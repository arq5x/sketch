#ifndef DNB_SKETCH_MULTIPLICITY_H__
#define DNB_SKETCH_MULTIPLICITY_H__

#include "blaze/Math.h"
#include <random>
#include "ccm.h" // Count-min sketch
#include <cstdarg>
#include <mutex>

namespace sketch {
using namespace common;
#ifndef LOG_DEBUG
#    define UNDEF_LDB
#    if !NDEBUG
#        define LOG_DEBUG(...) log_debug(__PRETTY_FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__)
//#include <cstdarg>
static int log_debug(const char *func, const char *filename, int line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret(std::fprintf(stderr, "[D:%s:%s:%d] ", func, filename, line));
    ret += std::vfprintf(stderr, fmt, args);
    va_end(args);
    return ret;
}
#    else
#        define LOG_DEBUG(...)
#    endif
#endif

namespace cws {

template<typename FType=float>
struct CWSamples {
    using MType = blaze::DynamicMatrix<float>;
    MType r_, c_, b_;
    CWSamples(size_t nhist, size_t histsz, uint64_t seed=0xB0BAFe77C001D00D): r_(nhist, histsz), c_(nhist, histsz), b_(nhist, histsz) {
        std::mt19937_64 mt(seed);
        std::gamma_distribution<FType> dist(2, 1);
        std::uniform_real_distribution<FType> rdist;
        for(size_t i = 0; i < nhist; ++i) {
            auto rr = row(r_, i);
            auto cr = row(c_, i);
            auto br = row(b_, i);
            for(size_t j = 0; j < histsz; ++j)
                rr[j] = dist(mt), cr[j] = dist(mt), br[j] = rdist(mt);
        }
    }
};

template<typename FType=float, typename HashStruct=common::WangHash, bool decay=false, bool conservative=false>
class realccm_t: public cm::ccmbase_t<cm::update::Increment,std::vector<FType, Allocator<FType>>,HashStruct,conservative> {
    using super = cm::ccmbase_t<cm::update::Increment,std::vector<FType, Allocator<FType>>,HashStruct,conservative>;
    using FSpace = vec::SIMDTypes<FType>;
    using super::seeds_;
    using super::data_;
    using super::nhashes_;
    using super::mask_;
    using super::subtbl_sz_;
    using super::l2sz_;
    using super::hash;
    static constexpr size_t rescale_frequency_ = 1ul << 12;

    FType scale_, scale_inv_, scale_cur_;
    std::atomic<uint64_t> total_added_;
    std::mutex mut_;
public:
    FType decay_rate() const {return scale_;}
    void addh(uint64_t val, FType inc=1.) {this->add(val, inc);}
    template<typename...Args>
    realccm_t(FType scale_prod, Args &&...args): scale_(scale_prod), scale_inv_(1./scale_prod), scale_cur_(scale_prod), super(std::forward<Args>(args)...) {
        total_added_.store(0);
        assert(scale_ >= 0. && scale_ <= 1.);
    }
    realccm_t(): realccm_t(1.-1e-7) {}
    void rescale(size_t exp=rescale_frequency_) {
        auto scale_div = std::pow(scale_, rescale_frequency_);
        auto ptr = reinterpret_cast<typename FSpace::VType *>(this->data_.data());
        auto eptr = reinterpret_cast<typename FSpace::VType *>(this->data_.data() + this->data_.size());
        auto mul = FSpace::set1(scale_div);
        while(eptr > ptr) {
            *ptr = Space::mul(ptr->simd_, mul);
            ++ptr;
        }
        FType *rptr = reinterpret_cast<FType *>(ptr);
        while(rptr < this->data_.data() + this->data_.size())
            *rptr++ *= scale_div;
    }
    FType add(const uint64_t val, FType inc) {
        ++total_added_; // I don't care about ordering, I just want it to be atomic.
        CONST_IF(decay) {
            inc *= scale_cur_;
            scale_cur_ *= scale_inv_;
            if(total_added_ % rescale_frequency_ == 0u) { // Power of two, bitmask is efficient
                {
                    std::lock_guard<decltype(mut_)> lock(mut_);
                    rescale();
                }
                scale_cur_ = scale_; // So when we multiply inc by scale_cur, the insertion happens at 1
            }
        }
        unsigned nhdone = 0, seedind = 0;
        const auto nperhash64 = lut::nhashesper64bitword[l2sz_];
        const auto nbitsperhash = l2sz_;
        const Type *sptr = reinterpret_cast<const Type *>(seeds_.data());
        Space::VType vb = Space::set1(val), tmp;
        FType ret;
        CONST_IF(conservative) {
            std::vector<uint64_t> indices, best_indices;
            indices.reserve(nhashes_);
            while(static_cast<int>(nhashes_) - static_cast<int>(nhdone) >= static_cast<ssize_t>(Space::COUNT * nperhash64))
                Space::VType(hash(Space::xor_fn(vb.simd_, Space::load(sptr++)))).for_each([&](uint64_t subval) {
                    for(unsigned k(0); k < nperhash64; indices.push_back(((subval >> (k++ * nbitsperhash)) & mask_) + nhdone++ * subtbl_sz_));
                }),
                seedind += Space::COUNT;
            while(nhdone < nhashes_) {
                uint64_t hv = hash(val ^ seeds_[seedind]);
                for(unsigned k(0); k < std::min(static_cast<unsigned>(nperhash64), nhashes_ - nhdone); indices.push_back(((hv >> (k++ * nbitsperhash)) & mask_) + subtbl_sz_ * nhdone++));
                ++seedind;
            }
            best_indices.push_back(indices[0]);
            ssize_t minval = data_[indices[0]];
            unsigned score;
            for(size_t i(1); i < indices.size(); ++i) {
                // This will change with
                if((score = data_[indices[i]]) == minval) {
                    best_indices.push_back(indices[i]);
                } else if(score < minval) {
                    best_indices.clear();
                    best_indices.push_back(indices[i]);
                    minval = score;
                }
            }
            ret = (data_[best_indices[0]] += inc);
            for(size_t i = 1; i < best_indices.size() - 1; data_[best_indices[i++]] += inc);
            // This is likely a scatter/gather candidate, but they aren't particularly fast operations.
            // This could be more valuable on a GPU.
        } else { // not conservative update. This means we support deletions
            ret = std::numeric_limits<decltype(ret)>::max();
            while(static_cast<int>(nhashes_) - static_cast<int>(nhdone) >= static_cast<ssize_t>(Space::COUNT * nperhash64)) {
                Space::VType(hash(Space::xor_fn(vb.simd_, Space::load(sptr++)))).for_each([&](uint64_t subval) {
                    for(unsigned k(0); k < nperhash64;) {
                        auto ref = data_[((subval >> (k++ * nbitsperhash)) & mask_) + nhdone++ * subtbl_sz_];
                        ref += inc;
                        ret = std::min(ret, double(ref));
                    }
                });
                seedind += Space::COUNT;
            }
            while(nhdone < nhashes_) {
                uint64_t hv = hash(val ^ seeds_[seedind++]);
                for(unsigned k(0); k < std::min(static_cast<unsigned>(nperhash64), nhashes_ - nhdone);) {
                    auto ref = data_[((hv >> (k++ * nbitsperhash)) & mask_) + nhdone++ * subtbl_sz_];
                    throw NotImplementedError("The updater should be here");
                    ret = std::min(ret, ssize_t(ref));
                }
            }
        }
        return ret;
    }
}; // realccm_t

} // namespace cws
namespace nt {
template<typename Container=std::vector<uint32_t, Allocator<uint32_t>>, typename HashStruct=WangHash, bool filter=true>
struct Card {
    // If using a different kind of counter than a native integer
    // simply define another numeric limits class providing max().
    // Ref: https://www.ncbi.nlm.nih.gov/pubmed/28453674
    using CounterType = std::decay_t<decltype(Container()[0])>;
    Container core_;
    const uint16_t p_, r_, pshift_;
    const CounterType maxcnt_;
    std::atomic<uint64_t> total_added_;
    HashStruct hf_;
    // Create a table of 2 << r entries
    // because the people who made this structure are weird
    // and use inconsistent notation
    template<typename...Args>
    Card(unsigned r, unsigned p, CounterType maxcnt, Args &&... args):
        core_(std::forward<Args>(args)...), p_(p), r_(r), pshift_(64 - p), maxcnt_(maxcnt) {
        total_added_.store(0);
        LOG_DEBUG("size of sketch: %zu\n", core_.size());
    }
    template<typename...Args>
    Card(unsigned r, unsigned p): Card(r, p, std::numeric_limits<CounterType>::max()) {}
    Card(Card &&o): core_(std::move(o.core_)), p_(o.p_), r_(o.r_), pshift_(o.pshift_), maxcnt_(o.maxcnt_), hf_(std::move(o.hf_)) {
        total_added_.store(o.total_added_.load());
    }
    void addh(uint64_t v) {
        v = hf_(v);
        add(v);
    }
    template<typename... Args>
    void set_hash(Args &&...args) {
        hf_ = std::move(HashStruct(std::forward<Args>(args)...));
    }
    size_t rbuck() const {
        return size_t(1) << r_; //
    }
    Card operator+(const Card &x) const {
        if(x.r_ != r_ || x.p_ != p_ || x.maxcnt_ != maxcnt_) {
            throw std::runtime_error("Parameter mismatch");
        }
        Card ret(r_, p_, maxcnt_, core_); // First, copy this container.
        ret += x;
        return ret;
    }
    Card &operator+=(const Card &x) {
        if(!std::is_same<Container, std::vector<CounterType, Allocator<CounterType>>>::value) {
            throw NotImplementedError("Haven't implemented merging of nthashes for any container but aligned std::vectors\n");
        }
        total_added_.store(total_added_.load() + x.total_added_.load());
        if(core_.size() * sizeof(core_[0]) >= sizeof(Space::VType)) {
            using VSpace = vec::SIMDTypes<CounterType>;
            using VType = typename vec::SIMDTypes<CounterType>::VType;
            VType *optr = reinterpret_cast<VType *>(this->core_.data());
            const VType *iptr = reinterpret_cast<const VType *>(x.core_.data());
            const VType *const eptr = reinterpret_cast<const VType *>(this->core_.data() + this->core_.size());
            assert(core_.size() % sizeof(VType) / sizeof(core_[0]) == 0);
            FOREVER {
                Space::store(reinterpret_cast<typename VSpace::Type *>(optr), Space::add(Space::load(reinterpret_cast<const typename VSpace::Type *>(optr)), Space::load(reinterpret_cast<const typename VSpace::Type *>(iptr))));
                ++iptr;
                ++optr;
                if(eptr == optr)
                    break;
            }
        } else for(size_t i = 0; i < core_.size(); core_[i] += x.core_[i], ++i);
        return *this;
    }
    void add(uint64_t v) {
        ++total_added_;
        const bool lastbit = v >> (pshift_ - 1) & 1;
        CONST_IF(filter) {
            if(v >> pshift_)
                return;
        }
        v <<= (64 - r_);
        v >>= (64 - r_);
        if(lastbit) v += (size_t(1) << r_);
        if(core_[v] != maxcnt_)
#ifndef NOT_THREADSAFE
            __sync_fetch_and_add(&core_[v], 1);
#else
            ++core_[v];
#endif
    }
    static constexpr double l2 = M_LN2;
    static constexpr size_t nsubs = 1ull << 16; // Why? The paper doesn't say and their code is weird.
    struct Deleter {
        template<typename T>
        void operator()(const T *x) const {
            std::free(const_cast<T *>(x));
        }
    };
    struct ResultType {
        std::vector<float> data_;
        size_t total;
        void report(std::FILE *fp=stderr) const {
            std::fprintf(fp, "maxcount=%zu,F1=%zu,F0=%f", data_.size() - 1, total, data_[0]);
            for(size_t i = 1; i < data_.size(); std::fprintf(fp, ",%f", data_[i++]));
        }
    };
#if !NDEBUG
#define access at
#else
#define access operator[]
#endif
    ResultType report() const {
        const CounterType max_val = *std::max_element(core_.begin(), core_.end()),
                          nvals = max_val + 1;
        std::vector<unsigned> arr(2 * nvals);
        LOG_DEBUG("Made arr with nvals = %zu\n", size_t(nvals));
        for(size_t i = 0; i < 2u; ++i) {
            size_t core_offset = i << r_;
            size_t arr_offset = nvals * i;
            std::fprintf(stderr, "offset for arr: %zu. cfor core: %zu\n", arr_offset, core_offset);
            for(size_t j = 0; j < size_t(1) << r_; ++j) {
                ++arr.access(core_.access(j + core_offset) + arr_offset);
            }
        }
        LOG_DEBUG("Filled arr with nvals = %zu\n", size_t(nvals));
        std::vector<double> pmeans(nvals);
        for(size_t i = 0; i < nvals; ++i) {
            pmeans[i] = (arr[i] + arr[i + nvals]) * .5;
        }
        //std::free(arr);
        std::vector<float> f_i(nvals);
        LOG_DEBUG("Made f_i arr\n");
        //if(!f_i) throw std::bad_alloc();
        double logpm0 = std::log(pmeans[0]);
        double lpmml2r = logpm0 - r_ * l2;
        f_i[0] = std::ldexp(-lpmml2r, p_ + r_); // F0 mean
        f_i[1]= -pmeans[1] / (pmeans[0] * (lpmml2r));
        for(size_t i = 2; i < nvals; ++i) {
            double sum=0.0;
            for(size_t j = 1; j < i; j++)
                sum += j * pmeans[i-j] * f_i[j];
            f_i[i] = -1.0*pmeans[i]/(pmeans[0]*(logpm0))-sum/(i*pmeans[0]);
        }
#undef access
        for(size_t i=1; i<nvals; f_i[i] = std::abs(f_i[i] * f_i[0]), ++i);
        return ResultType{std::move(f_i), total_added_.load()};
    }
}; // Card
template<typename CType, typename HashStruct=WangHash, bool filter=true>
struct VecCard: public Card<std::vector<CType, Allocator<CType>>, HashStruct, filter> {
    using super = Card<std::vector<CType, Allocator<CType>>, HashStruct, filter>;
    static_assert(std::is_integral<CType>::value, "Must be integral.");
    VecCard(unsigned r, unsigned p, CType max=std::numeric_limits<CType>::max()): super(r, p, max, 2ull << r) {} // 2 << r
};

} // namespace nt
} // namespace sketch

#ifdef UNDEF_LDB
#undef LOG_DEBUG
#endif

#endif /* DNB_SKETCH_MULTIPLICITY_H__ */
