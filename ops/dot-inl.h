// Copyright 2024 Google LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stddef.h>

#include <array>

#include "compression/compress.h"
#include "hwy/base.h"

// Include guard for (potentially) SIMD code.
#if defined(THIRD_PARTY_GEMMA_CPP_DOT_TOGGLE) == defined(HWY_TARGET_TOGGLE)
#ifdef THIRD_PARTY_GEMMA_CPP_DOT_TOGGLE
#undef THIRD_PARTY_GEMMA_CPP_DOT_TOGGLE
#else
#define THIRD_PARTY_GEMMA_CPP_DOT_TOGGLE
#endif

#include "hwy/highway.h"
// After highway.h
#include "compression/compress-inl.h"
#include "ops/fp_arith-inl.h"
#include "hwy/contrib/math/math-inl.h"
#include "hwy/profiler.h"  // also uses SIMD

HWY_BEFORE_NAMESPACE();
namespace gcpp {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

//------------------------------------------------------------------------------

// Returns 2 * sum(|w.*v|) / |sum(w.*v)|. This is large when there are many
// similar-magnitude and opposite-sign elements. See
// https://en.wikipedia.org/wiki/Condition_number.
template <typename WeightT, typename VecT>
HWY_MAYBE_UNUSED double ConditionNumber(const WeightT* HWY_RESTRICT w,
                                        const VecT* HWY_RESTRICT v,
                                        size_t num) {
  PROFILER_FUNC;
  const hn::ScalableTag<float> df;
  using VF = hn::Vec<decltype(df)>;
  const size_t N = hn::Lanes(df);

  VF sum = hn::Zero(df);
  VF sum_err = hn::Zero(df);
  VF sum_abs = hn::Zero(df);
  VF sum_abs_err = hn::Zero(df);

  const auto packed_w = MakeSpan(w, num);
  const auto packed_v = MakeSpan(v, num);

  size_t i = 0;
  if (num >= 2 * N) {
    for (; i <= num - 2 * N; i += 2 * N) {
      VF w0, w1, v0, v1;
      Decompress2(df, packed_w, i, w0, w1);
      Decompress2(df, packed_v, i, v0, v1);
      const VF mul0 = hn::Mul(w0, v0);
      const VF mul1 = hn::Mul(w1, v1);
      UpdateCascadedSums(df, mul0, sum, sum_err);
      UpdateCascadedSums(df, mul1, sum, sum_err);
      UpdateCascadedSums(df, hn::Abs(mul0), sum_abs, sum_abs_err);
      UpdateCascadedSums(df, hn::Abs(mul1), sum_abs, sum_abs_err);
    }
  }

  size_t remaining = num - i;
  HWY_DASSERT(remaining < 2 * N);
  if (HWY_UNLIKELY(remaining != 0)) {
    HWY_ALIGN float padded_w[2 * hn::MaxLanes(df)];
    HWY_ALIGN float padded_v[2 * hn::MaxLanes(df)];
    DecompressAndZeroPad(df, packed_w, i, padded_w, remaining);
    DecompressAndZeroPad(df, packed_v, i, padded_v, remaining);

    // 1..2 whole vectors, possibly zero-padded.
    for (size_t padded_pos = 0; padded_pos < remaining; padded_pos += N) {
      const VF w0 = hn::Load(df, padded_w + padded_pos);
      const VF v0 = hn::Load(df, padded_v + padded_pos);
      const VF mul = hn::Mul(w0, v0);
      UpdateCascadedSums(df, mul, sum, sum_err);
      UpdateCascadedSums(df, hn::Abs(mul), sum_abs, sum_abs_err);
    }
  }

  const float div = hwy::ScalarAbs(ReduceCascadedSums(df, sum, sum_err));
  if (div == 0.0f) return hn::GetLane(hn::Inf(df));
  const double cond = 2.0 * ReduceCascadedSums(df, sum_abs, sum_abs_err) /
                      static_cast<double>(div);
  HWY_ASSERT(cond >= 0.0);
  return cond;
}

// Same, but for a single vector - just skips the product.
template <typename VecT>
HWY_MAYBE_UNUSED double ConditionNumber(const VecT* HWY_RESTRICT v,
                                        size_t num) {
  PROFILER_FUNC;
  const hn::ScalableTag<float> df;
  using VF = hn::Vec<decltype(df)>;
  const size_t N = hn::Lanes(df);

  VF sum = hn::Zero(df);
  VF sum_err = hn::Zero(df);
  VF sum_abs = hn::Zero(df);
  VF sum_abs_err = hn::Zero(df);

  const auto packed_v = MakeSpan(v, num);

  size_t i = 0;
  if (num >= 2 * N) {
    for (; i <= num - 2 * N; i += 2 * N) {
      VF v0, v1;
      Decompress2(df, packed_v, i, v0, v1);
      UpdateCascadedSums(df, v0, sum, sum_err);
      UpdateCascadedSums(df, v1, sum, sum_err);
      UpdateCascadedSums(df, hn::Abs(v0), sum_abs, sum_abs_err);
      UpdateCascadedSums(df, hn::Abs(v1), sum_abs, sum_abs_err);
    }
  }

  size_t remaining = num - i;
  HWY_DASSERT(remaining < 2 * N);
  if (HWY_UNLIKELY(remaining != 0)) {
    HWY_ALIGN float padded_v[2 * hn::MaxLanes(df)];
    DecompressAndZeroPad(df, packed_v, i, padded_v, remaining);

    // 1..2 whole vectors, possibly zero-padded.
    for (size_t padded_pos = 0; padded_pos < remaining; padded_pos += N) {
      const VF v0 = hn::Load(df, padded_v + padded_pos);
      UpdateCascadedSums(df, v0, sum, sum_err);
      UpdateCascadedSums(df, hn::Abs(v0), sum_abs, sum_abs_err);
    }
  }

  const float div = hwy::ScalarAbs(ReduceCascadedSums(df, sum, sum_err));
  if (div == 0.0f) return hn::GetLane(hn::Inf(df));
  const double cond = 2.0 * ReduceCascadedSums(df, sum_abs, sum_abs_err) /
                      static_cast<double>(div);
  HWY_ASSERT(cond >= 0.0);
  return cond;
}

// Algorithm 6.15 from Handbook of Floating-Point Arithmetic. 10 ops is too slow
// for compute-limited Matmul but might be OK for attention.
// Also supports bf16 inputs, used by matvec-inl.h.
struct DotKernelCompensated {
  template <class DF, class VF = hn::Vec<DF>, HWY_IF_F32_D(DF)>
  HWY_INLINE void Update4(DF df, const VF w0, const VF w1, const VF w2,
                          const VF w3, const VF v0, const VF v1, const VF v2,
                          const VF v3, VF& sum0, VF& sum1, VF& sum2, VF& sum3,
                          VF& comp0, VF& comp1, VF& comp2, VF& comp3) const {
    VF perr0, perr1, perr2, perr3;
    const VF prod0 = TwoProducts(df, w0, v0, perr0);
    const VF prod1 = TwoProducts(df, w1, v1, perr1);
    const VF prod2 = TwoProducts(df, w2, v2, perr2);
    const VF prod3 = TwoProducts(df, w3, v3, perr3);

    VF serr0, serr1, serr2, serr3;
    sum0 = TwoSums(df, prod0, sum0, serr0);
    sum1 = TwoSums(df, prod1, sum1, serr1);
    sum2 = TwoSums(df, prod2, sum2, serr2);
    sum3 = TwoSums(df, prod3, sum3, serr3);

    comp0 = hn::Add(comp0, hn::Add(perr0, serr0));
    comp1 = hn::Add(comp1, hn::Add(perr1, serr1));
    comp2 = hn::Add(comp2, hn::Add(perr2, serr2));
    comp3 = hn::Add(comp3, hn::Add(perr3, serr3));
  }

  template <class DBF, class VBF = hn::Vec<DBF>, HWY_IF_BF16_D(DBF),
            class DF = hn::Repartition<float, DBF>, class VF = hn::Vec<DF>>
  HWY_INLINE void Update4(DBF /*dbf*/, const VBF w0, const VBF w1, const VBF w2,
                          const VBF w3, const VBF v0, const VBF v1,
                          const VBF v2, const VBF v3, VF& sum0, VF& sum1,
                          VF& sum2, VF& sum3, VF& comp0, VF& comp1, VF& comp2,
                          VF& comp3) const {
    const DF df;
    const VF prod0 = WidenMulPairwiseAdd(df, w0, v0);
    const VF prod1 = WidenMulPairwiseAdd(df, w1, v1);
    const VF prod2 = WidenMulPairwiseAdd(df, w2, v2);
    const VF prod3 = WidenMulPairwiseAdd(df, w3, v3);

    VF serr0, serr1, serr2, serr3;
    sum0 = TwoSums(df, prod0, sum0, serr0);
    sum1 = TwoSums(df, prod1, sum1, serr1);
    sum2 = TwoSums(df, prod2, sum2, serr2);
    sum3 = TwoSums(df, prod3, sum3, serr3);

    comp0 = hn::Add(comp0, serr0);
    comp1 = hn::Add(comp1, serr1);
    comp2 = hn::Add(comp2, serr2);
    comp3 = hn::Add(comp3, serr3);
  }

  template <class DF, class VF = hn::Vec<DF>, HWY_IF_F32_D(DF)>
  HWY_INLINE void Update1(DF df, const VF w0, const VF v0, VF& sum0,
                          VF& comp0) const {
    VF perr0;
    const VF prod0 = TwoProducts(df, w0, v0, perr0);

    VF serr0;
    sum0 = TwoSums(df, prod0, sum0, serr0);

    comp0 = hn::Add(comp0, hn::Add(perr0, serr0));
  }

  template <class DBF, class VBF = hn::Vec<DBF>, HWY_IF_BF16_D(DBF),
            class DF = hn::Repartition<float, DBF>, class VF = hn::Vec<DF>>
  HWY_INLINE void Update1(DBF /*dbf*/, const VBF w0, const VBF v0, VF& sum0,
                          VF& comp0) const {
    const DF df;
    const VF prod0 = WidenMulPairwiseAdd(df, w0, v0);

    VF serr0;
    sum0 = TwoSums(df, prod0, sum0, serr0);

    comp0 = hn::Add(comp0, serr0);
  }

  template <class DF, class VF = hn::Vec<DF>>
  HWY_INLINE float Reduce(DF df, VF& sum0, VF& sum1, VF& sum2, VF& sum3,
                          VF& comp0, VF& comp1, VF& comp2, VF& comp3) const {
    // Reduction tree: sum of all accumulators by pairs, then across lanes.
    AssimilateCascadedSums(df, sum1, comp1, sum0, comp0);
    AssimilateCascadedSums(df, sum3, comp3, sum2, comp2);
    AssimilateCascadedSums(df, sum2, comp2, sum0, comp0);
    return ReduceCascadedSums(df, sum0, comp0);
  }
};

// Default kernel
template <class D, typename WeightT, typename VecT>
HWY_INLINE float Dot(D d, const PackedSpan<const WeightT>& w, size_t w_ofs,
                     const VecT* HWY_RESTRICT vec, size_t num) {
  return DecompressAndCall(d, w, w_ofs, MakeSpan(vec, num),
                           DotKernelCompensated());
}

// Adapter for a single pointer, no bounds checking.
template <typename WeightT, typename VecT>
HWY_INLINE float Dot(const WeightT* HWY_RESTRICT w, const VecT* vec,
                     size_t num) {
  const hn::ScalableTag<VecT> d;
  return Dot(d, MakeConstSpan(w, num), /*w_ofs=*/0, vec, num);
}

// Adapter for use by matvec-inl.h. TODO: remove when that is no longer used.
template <size_t kCapacity, typename VecT>
HWY_INLINE float Dot(const std::array<float, kCapacity>& w, size_t w_ofs,
                     const VecT* vec, size_t num) {
  const hn::ScalableTag<VecT> d;
  return Dot(d, MakeConstSpan(w.data(), kCapacity), w_ofs, vec, num);
}

// Adapter for use by matvec-inl.h. TODO: remove when that is no longer used.
template <typename MatT, size_t kCapacity, typename VecT>
HWY_INLINE float Dot(const CompressedArray<MatT, kCapacity>& w, size_t w_ofs,
                     const VecT* vec, size_t num) {
  const hn::ScalableTag<VecT> d;
  return w.scale() *
         Dot(d, MakeConstSpan(w.data(), kCapacity), w_ofs, vec, num);
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace gcpp
HWY_AFTER_NAMESPACE();

#endif  // NOLINT