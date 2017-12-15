#ifndef _GFRP_KERNEL_H__
#define _GFRP_KERNEL_H__
#include "frp/spinner.h"

namespace frp {

namespace ff {

struct GaussianFinalizer {
private:
    uint32_t use_lowprec_:1;
public:
    GaussianFinalizer(bool use_low_precision=false): use_lowprec_(use_low_precision) {}
    void set_use_lowprec(bool use_lowprec) {use_lowprec_ = use_lowprec;}

    template<typename VecType>
    void apply(VecType &in) const {
        if((in.size() & (in.size() - 1))) std::fprintf(stderr, "in.size() [%zu] is not a power of 2.\n", in.size()), exit(1);
        using FloatType = typename std::decay_t<decltype(in[0])>;
        using SIMDType  = vec::SIMDTypes<FloatType>;
        using VT = typename SIMDType::Type;
        using DT = typename SIMDType::TypeDouble;
        static const size_t ratio(sizeof(VT) / sizeof(FloatType));
        DT dest;
        VT *srcptr((VT *)&in[0]);
        if(use_lowprec_) {
            if constexpr(IS_BLAZE(VecType)) {
                for(u32 i((in.size() >> 1) / ratio); i;) {
                    dest = SIMDType::sincos_u35(SIMDType::load((FloatType *)&srcptr[i - 1]));
                    SIMDType::store((FloatType *)&srcptr[(i << 1) - 1], dest.y);
                    SIMDType::store((FloatType *)&srcptr[--i << 1], dest.x);
                }
            } else {
                if(SIMDType::aligned(srcptr)) {
                    for(u32 i((in.size() >> 1) / ratio); i;) {
                        dest = SIMDType::sincos_u35(SIMDType::load((FloatType *)&srcptr[i - 1]));
                        SIMDType::store((FloatType *)&srcptr[(i << 1) - 1], dest.y);
                        SIMDType::store((FloatType *)&srcptr[--i << 1], dest.x);
                    }
                } else {
                    for(u32 i((in.size() >> 1) / ratio); i;) {
                        dest = SIMDType::sincos_u35(SIMDType::loadu((FloatType *)&srcptr[i - 1]));
                        SIMDType::storeu((FloatType *)&srcptr[(i << 1) - 1], dest.y);
                        SIMDType::storeu((FloatType *)&srcptr[--i << 1], dest.x);
                    }
                }
            }
        } else {
            if constexpr(IS_BLAZE(VecType)) {
                for(u32 i((in.size() >> 1) / ratio); i;) {
                    dest = SIMDType::sincos_u10(SIMDType::load((FloatType *)&srcptr[i - 1]));
                    SIMDType::store((FloatType *)&srcptr[(i << 1) - 1], dest.y);
                    SIMDType::store((FloatType *)&srcptr[--i << 1], dest.x);
                }
            } else {
                if(SIMDType::aligned(srcptr)) {
                    for(u32 i((in.size() >> 1) / ratio); i;) {
                        dest = SIMDType::sincos_u10(SIMDType::load((FloatType *)&srcptr[i - 1]));
                        SIMDType::store((FloatType *)&srcptr[(i << 1) - 1], dest.y);
                        SIMDType::store((FloatType *)&srcptr[--i << 1], dest.x);
                    }
                } else {
                    for(u32 i((in.size() >> 1) / ratio); i;) {
                        dest = SIMDType::sincos_u10(SIMDType::loadu((FloatType *)&srcptr[i - 1]));
                        SIMDType::storeu((FloatType *)&srcptr[(i << 1) - 1], dest.y);
                        SIMDType::storeu((FloatType *)&srcptr[--i << 1], dest.x);
                    }
                }
            }
        }
    }
};


template<typename FloatType, typename RademType=CompactRademacher>
class FastFoodKernelBlock {
    size_t final_output_size_; // This is twice the size passed to the Hadamard transforms
    using RandomScalingBlock = RandomGammaIncInvScalingBlock<FloatType>;
    using SizeType = uint32_t;
    using Shuffler = LutShuffler<SizeType>;
    using SpinTransformer =
        SpinBlockTransformer<FastFoodGaussianProductBlock<FloatType>,
                             RandomScalingBlock, HadamardBlock,
                             UnitGaussianScalingBlock<FloatType>, Shuffler, HadamardBlock,
                             RademType>;
    SpinTransformer tx_;

public:
    using float_type = FloatType;
    using GaussianMatrixType = UnitGaussianScalingBlock<FloatType>;
    FastFoodKernelBlock(size_t size, FloatType sigma=1., uint64_t seed=-1, bool renorm=true):
        final_output_size_(size),
        tx_(
            std::make_tuple(FastFoodGaussianProductBlock<FloatType>(sigma),
                   RandomScalingBlock(seed + seed * seed - size * size, size),
                   HadamardBlock(size, renorm),
                   GaussianMatrixType(seed * seed, size),
                   Shuffler(size, seed),
                   HadamardBlock(size, renorm),
                   RademType(size, (seed ^ (size * size)) + seed)))
    {
        if(final_output_size_ & (final_output_size_ - 1))
            throw std::runtime_error((std::string(__PRETTY_FUNCTION__) + "'s size should be a power of two.").data());
        auto &rsbref(std::get<RandomScalingBlock>(tx_.get_tuple()));
        auto &gmref(std::get<GaussianMatrixType>(tx_.get_tuple()));
        rsbref.rescale(1./std::sqrt(gmref.vec_norm()));
    }
    size_t transform_size() const {return final_output_size_;}
    template<typename InputType, typename OutputType>
    void apply(OutputType &out, const InputType &in) {
#if 0
        if(out.size() != final_output_size_) {
            fprintf(stderr, "Warning: Output size was wrong (%zu, not %zu). Resizing\n", out.size(), final_output_size_);
        }
#endif
        if(roundup(in.size()) != transform_size()) throw std::runtime_error("ZOMG");
        blaze::reset(out);

        subvector(out, 0, in.size()) = in;
        auto half_vector(subvector(out, 0, transform_size()));
        tx_.apply(half_vector);
    }
};

template<typename KernelBlock,
         typename Finalizer=GaussianFinalizer>
class Kernel {
    std::vector<KernelBlock> blocks_;
    Finalizer             finalizer_;

public:
    using FloatType = typename KernelBlock::float_type;
    template<typename... Args>
    Kernel(size_t stacked_size, size_t input_size,
           FloatType sigma, uint64_t seed,
           Args &&... args):
        finalizer_(std::forward<Args>(args)...)
    {
        size_t input_ru = roundup(input_size);
        stacked_size = std::max(stacked_size, input_ru);
        if(stacked_size % input_ru)
            stacked_size = input_ru - (stacked_size % input_ru);
        if(stacked_size % input_ru) std::fprintf(stderr, "Stacked size is not evenly divisible.\n"), exit(1);
        size_t nblocks = (stacked_size) / input_ru;
        aes::AesCtr gen(seed);
        while(blocks_.size() < nblocks) {
            blocks_.emplace_back(input_ru, sigma, gen());
        }
    }
    template<typename InputType, typename OutputType>
    void apply(OutputType &out, const InputType &in) {
        size_t in_rounded(roundup(in.size()));
        if(out.size() != (blocks_.size() << 1) * in_rounded) {
            if constexpr(blaze::IsView<OutputType>::value) {
                throw std::runtime_error(ks::sprintf("[%s] Resizing out block from %zu to %zu to match %zu input and %zu rounded up input.\n",
                                                     __PRETTY_FUNCTION__, out.size(), (blocks_.size() << 1) * in_rounded, in.size(), (size_t)roundup(in.size())).data());
            } else {
                std::fprintf(stderr, "Resizing out block from %zu to %zu to match %zu input and %zu rounded up input.\n",
                             out.size(), (blocks_.size() << 1) * in_rounded, in.size(), (size_t)roundup(in.size()));
                out.resize((blocks_.size() << 1) * in_rounded);
            }
        }
        //in_rounded <<= 1; // To account for the doubling for the sin/cos entry for each random projection.
#if 0
        #pragma omp parallel for
#endif
        for(size_t i = 0; i < blocks_.size(); ++i) {
            auto sv(subvector(out, (in_rounded << 1) * i, in_rounded));
            blocks_[i].apply(sv, in);
            finalizer_.apply(sv);
        }
        vec::blockmul(out, 1./std::sqrt(static_cast<FloatType>(out.size() >> 1)));
    }
};

} // namespace ff


} // namespace frp

#endif // #ifndef _GFRP_KERNEL_H__
