#ifndef COUNTING_CRUEL_BLOOM__
#define COUNTING_CRUEL_BLOOM__
#include "bf.h"
#include "aesctr/aesctr.h"

namespace sketch {
namespace bf {

template<typename HashStruct=WangHash, typename RngType=aes::AesCtr<std::uint64_t, 8>>
class cbfbase_t {
protected:
    std::vector<bfbase_t<HashStruct>> bfs_;
    RngType   rng_;
    uint64_t  gen_;
    uint8_t nbits_;
    // TODO: this can be improved by providing a continuous chunk of memory
    //       and performing all operations on subfilters.
public:
    explicit cbfbase_t(const std::vector<unsigned> &l2szs, unsigned nhashes, uint64_t seedseedseedval): rng_{seedseedseedval}, gen_(rng_()), nbits_(64) {
        if(l2szs.empty()) throw std::runtime_error("Need at least 1 size for hashes.");
        bfs_.reserve(l2szs.size());
        std::generate_n(std::back_inserter(bfs_), l2szs.size(), [&]{return bfbase_t<HashStruct>(l2szs[bfs_.size()], nhashes, rng_());});
    }
    explicit cbfbase_t(size_t nbfs, size_t l2sz, unsigned nhashes, uint64_t seedseedseedval):
        cbfbase_t(std::vector<unsigned>(nbfs, l2sz),  nhashes, seedseedseedval) {}
    INLINE void addh(const uint64_t val) {
        auto it(bfs_.begin());
        if(!it->may_contain(val)) {
            it->addh(val);
            return;
        }
        FOREVER {
            ++it;
            if(it == bfs_.end())     return;
            if(!it->may_contain(val)) break;
        }
        if(it == bfs_.end()) return; // Already at capacity
        // Otherwise, probabilistically insert at position.
        const auto dist = static_cast<unsigned>(std::distance(bfs_.begin(), it));
        if(__builtin_expect(nbits_ < dist, 0)) gen_ = rng_(), nbits_ = 64;
        if((gen_ & (UINT64_C(-1) >> (64 - dist))) == 0) it->addh(val); // Flip the biased coin, add if it returns 'heads'
        gen_ >>= dist, nbits_ -= dist;
    }
    bool may_contain(uint64_t val) const {
        return bfs_[0].may_contain(val);
    }
    unsigned est_count(const uint64_t val) const {
        auto it(bfs_.cbegin());
        if(!it->may_contain(val)) return 0;
        for(++it;it < bfs_.end() && it->may_contain(val); ++it);
        return 1u << (std::distance(bfs_.cbegin(), it) - 1);
    }
    void resize_sketches(unsigned np) {
        for(auto &bf: bfs_) bf.resize(np);
    }
    void resize(unsigned nbfs) {
        bfs_.reserve(nbfs);
        auto nhashes = bfs_.at(0).nhashes();
        auto np = bfs_[0].p();
        for(auto &bf: bfs_) bf.clear();
        while(bfs_.size() < nbfs) bfs_.emplace_back(np, nhashes, rng_());
    }
    void clear() {
        for(auto &bf: bfs_) bf.clear();
    }
    auto p() const {
        return bfs_[0].p();
    }
    auto nhashes() const {
        return bfs_[0].nhashes();
    }
    std::size_t size() const {return bfs_.size();}
    std::size_t filter_size() const {return bfs_[0].size();}
    auto begin() const {return bfs_.cbegin();}
    auto end() const {return bfs_.cend();}
};
using cbf_t = cbfbase_t<>;


namespace detail {
std::vector<unsigned> pcbf_hll_pgen(unsigned nsketches, unsigned l2sz, unsigned hllp=0, bool shrinkpow2=true) {
    std::vector<unsigned> ret; ret.reserve(nsketches);
    unsigned p = hllp ? hllp: std::max(l2sz - 4, 8u);
    std::generate_n(std::back_inserter(ret), nsketches, [&](){
        auto ret = std::max(8u, p);
        p -= shrinkpow2;
        return ret;
    });
    return ret;
}
} // namespace detail

template<typename HashStruct=WangHash, typename RngType=aes::AesCtr<uint64_t, 8>>
class pcbfbase_t {
    // Probabilistic bloom filter counting.
    // Much like cbf_t, but also provides cardinality estimates for the number of elements reaching each stage.
protected:
    using bf_t  = bf::bfbase_t<HashStruct>;
    using hll_t = hll::seedhllbase_t<HashStruct>;

    std::vector<hll_t> hlls_;
    std::vector<bf_t>   bfs_;
    RngType             rng_;
    uint64_t            gen_;
    uint8_t           nbits_;
public:
    explicit pcbfbase_t(const std::vector<unsigned> &l2szs, const std::vector<unsigned> &hllps, unsigned nhashes,
                        uint64_t seedseedseedval, hll::EstimationMethod estim=hll::ERTL_MLE,
                        hll::JointEstimationMethod jestim=hll::ERTL_JOINT_MLE):
        rng_{seedseedseedval}, gen_(rng_()), nbits_(64)
    {
        bfs_.reserve(l2szs.size()), hlls_.reserve(l2szs.size());
        std::generate_n(std::back_inserter(bfs_), l2szs.size(), [&](){
            return bf_t(l2szs[bfs_.size()], nhashes, rng_());
        });
        std::generate_n(std::back_inserter(hlls_), l2szs.size(), [&](){
            return hll_t(rng_(), hllps[hlls_.size()], estim, jestim, 1, false);
        });
    }
    explicit pcbfbase_t(size_t nbfs, size_t l2sz, unsigned nhashes,
                        uint64_t seedseedseedval, unsigned hllp=0, hll::EstimationMethod estim=hll::ERTL_MLE,
                        hll::JointEstimationMethod jestim=hll::ERTL_JOINT_MLE, bool shrinkpow2=true):
        pcbfbase_t(std::vector<unsigned>(nbfs, l2sz),
                   detail::pcbf_hll_pgen(nbfs, l2sz, hllp, shrinkpow2),
                   nhashes, seedseedseedval, estim, jestim) {}
    const std::vector<bf_t>  &bfs()  const {return bfs_;}
    const std::vector<hll_t> &hlls() const {return hlls_;}
    void resize_bloom(unsigned newsize) {for(auto &bf: bfs_) bf.resize(newsize);}
    size_t size() const {return bfs_.size();}
    INLINE void addh(uint64_t val) {
        if(!bfs_[0].may_contain(val) || !hlls_[0].may_contain(val)) {
            bfs_[0].addh(val), hlls_[0].addh(val);
            return;
        }
        unsigned i(1);
        FOREVER {
            if(i == bfs_.size()) return;
            if(!bfs_[i].may_contain(val) || !hlls_[i].may_contain(val)) break;
            ++i;
        }
        if(__builtin_expect(nbits_ < i, 0)) gen_ = rng_(), nbits_ = 64;
        if((gen_ & (UINT64_C(-1) >> (64 - i))) == 0) bfs_[i].addh(val), hlls_[i].addh(val);
        gen_ >>= i, nbits_ -= i;
    }
    INLINE void addh(VType val) {
        val.for_each([&](uint64_t val) {this->addh(val);}); // Could be further accelerated with SIMD. I'm including this for interface compatibility.
    }
    bool may_contain(uint64_t val) const {
        for(unsigned i(0); i < bfs_.size(); ++i) if(!bfs_[i].may_contain(val) || !hlls_[i].may_contain) return false;
        return true;
    }
    void clear() {
        for(auto &h: hlls_) h.clear();
        for(auto &b: bfs_)  b.clear();
        gen_ = nbits_ = 0;
    }
    unsigned naive_est_count(uint64_t val) const {
        if(!bfs_[0].may_contain(val) || !hlls_[0].may_contain(val)) return 0;
        unsigned i(1);
        while(i != bfs_.size() && bfs_[i].may_contain(val) && hlls_[i].may_contain(val)) ++i;
        return 1u << (i - 1);
    }
    unsigned est_count(uint64_t val) const {return naive_est_count(val);}
};

using pcbf_t = pcbfbase_t<hll::WangHash>;

} // namespace bf
} // namespace sketch

#endif // #ifndef COUNTING_CRUEL_BLOOM__
