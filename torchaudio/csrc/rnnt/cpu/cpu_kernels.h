#pragma once

#include <torchaudio/csrc/rnnt/cpu/alignment_restrictions.h>
#include <torchaudio/csrc/rnnt/cpu/math.h>
#include <torchaudio/csrc/rnnt/options.h>
#include <torchaudio/csrc/rnnt/types.h>

#include <cstring>
#include <limits>
#include <vector>

namespace torchaudio {
namespace rnnt {
namespace cpu {

template <typename DTYPE>
struct LogProbs {
  DTYPE skip_; // blank.
  DTYPE emit_; // target.

  LogProbs(DTYPE skip, DTYPE emit) : skip_(skip), emit_(emit) {}

  DTYPE& skip() {
    return skip_;
  }
  DTYPE& emit() {
    return emit_;
  }

  const DTYPE& skip() const {
    return skip_;
  }
  const DTYPE& emit() const {
    return emit_;
  }
};

// TensorView: view a block of allocated memory as a tensor.
template <typename DTYPE>
class TensorView {
 public:
  TensorView(const std::vector<int>& dims, DTYPE* data)
      : dims_(dims), data_(data) {
    strides_.resize(dims.size());
    strides_.back() = 1;
    for (int i = dims.size() - 2; i >= 0; --i) {
      strides_[i] = strides_[i + 1] * dims[i + 1];
    }
  }

  DTYPE& operator()(const std::vector<int>& indices) {
    CHECK_EQ(indices.size(), dims_.size());
    int index = indices.back();
    for (int i = indices.size() - 2; i >= 0; --i) {
      index += indices[i] * strides_[i];
    }
    return data_[index];
  }

  void SetZero() {
    int size = dims_[0] * strides_[0];
    std::memset(data_, 0, sizeof(DTYPE) * size);
  }

 private:
  std::vector<int> dims_;
  std::vector<int> strides_;
  DTYPE* data_;
};

template <typename DTYPE, typename CAST_DTYPE>
status_t LogSumExp2D(int N, int D, const DTYPE* logits, CAST_DTYPE* outputs) {
  for (int i = 0; i < N * D; i += D) {
    CAST_DTYPE max = logits[i];
    for (int j = 1; j < D; ++j) {
      max = std::max(max, CAST_DTYPE(logits[i + j]));
    }
    CAST_DTYPE sum = 0;
    for (int j = 0; j < D; ++j) {
      sum = sum + std::exp(CAST_DTYPE(logits[i + j]) - max);
    }
    outputs[i / D] = max + std::log(sum);
  }

  return SUCCESS;
}

template <typename DTYPE, typename CAST_DTYPE>
void ComputeLogProbsOneSequence(
    const Options& options,
    TensorView<const DTYPE>& logits,
    const int* targets,
    int srcLen,
    int tgtLen,
    TensorView<const CAST_DTYPE>& denom,
    TensorView<LogProbs<CAST_DTYPE>>& logProbs) {
  const int& T = srcLen;
  const int& U = tgtLen;
  const int& blank = options.blank_;

  for (int t = 0; t < T; ++t) {
    for (int u = 0; u < U; ++u) {
      if (u < U - 1) {
        logProbs({t, u}).emit() =
            CAST_DTYPE(logits({t, u, targets[u]})) - denom({t, u});
      }
      logProbs({t, u}).skip() =
          CAST_DTYPE(logits({t, u, blank})) - denom({t, u});
    }
  }
}

template <typename DTYPE, typename CAST_DTYPE>
status_t ComputeLogProbs(
    const Options& options,
    const DTYPE* logits,
    const int* targets,
    const int* srcLengths,
    const int* tgtLengths,
    const CAST_DTYPE* denominators,
    CAST_DTYPE* logProbs) {
  std::vector<TensorView<const DTYPE>> seqLogits;
  std::vector<const int*> seqTargets;
  std::vector<TensorView<const CAST_DTYPE>> seqDenoms;
  std::vector<TensorView<LogProbs<CAST_DTYPE>>> seqlogProbs;

  const int& B = options.batchSize_;
  const int& maxT = options.maxSrcLen_;
  const int& maxU = options.maxTgtLen_;
  const int& D = options.numTargets_;
  for (int b = 0; b < B; ++b) {
    seqLogits.push_back(
        TensorView<const DTYPE>({maxT, maxU, D}, logits + b * maxT * maxU * D));
    seqTargets.push_back(targets + b * (maxU - 1));
    seqDenoms.push_back(TensorView<const CAST_DTYPE>(
        {maxT, maxU}, denominators + b * maxT * maxU));
    seqlogProbs.push_back(TensorView<LogProbs<CAST_DTYPE>>(
        {maxT, maxU},
        reinterpret_cast<LogProbs<CAST_DTYPE>*>(logProbs) + b * maxT * maxU));
  }

  //#pragma omp parallel for
  for (int b = 0; b < B; ++b) { // use max 2 * B threads.
    ComputeLogProbsOneSequence<DTYPE, CAST_DTYPE>(
        /*options=*/options,
        /*logits=*/seqLogits[b],
        /*targets=*/seqTargets[b],
        /*srcLen=*/srcLengths[b],
        /*tgtLen=*/tgtLengths[b] + 1, // with prepended blank.
        /*denom=*/seqDenoms[b],
        /*logProbs=*/seqlogProbs[b]);
  }

  return SUCCESS;
}

template <typename DTYPE>
DTYPE ComputeAlphaOneSequence(
    const Options& options,
    TensorView<const LogProbs<DTYPE>>& logProbs,
    int srcLen,
    int tgtLen,
    TensorView<DTYPE>& alpha) {
  const int& T = srcLen;
  const int& U = tgtLen;

  alpha({0, 0}) = DTYPE(0);

  for (int t = 1; t < T; ++t) { // u == 0.
    alpha({t, 0}) = alpha({t - 1, 0}) + logProbs({t - 1, 0}).skip();
  }

  for (int u = 1; u < U; ++u) { // t == 0.
    alpha({0, u}) = alpha({0, u - 1}) + logProbs({0, u - 1}).emit();
  }

  for (int t = 1; t < T; ++t) {
    for (int u = 1; u < U; ++u) {
      alpha({t, u}) = math::lse(
          alpha({t - 1, u}) + logProbs({t - 1, u}).skip(),
          alpha({t, u - 1}) + logProbs({t, u - 1}).emit());
    }
  }

  DTYPE forward_score = alpha({T - 1, U - 1}) + logProbs({T - 1, U - 1}).skip();

  return forward_score;
}

template <typename DTYPE>
DTYPE ComputeAlphaOneSequenceRestricted(
    const Options& options,
    TensorView<const LogProbs<DTYPE>>& logProbs,
    int srcLen,
    int tgtLen,
    TensorView<DTYPE>& alpha,
    const int* wpEnds) {
  const int& T = srcLen;
  const int& U = tgtLen;

  const int& lBuffer = options.lBuffer_;
  const int& r_buffer = options.rBuffer_;

  AlignmentRestrictionCheck check(wpEnds, T, U, lBuffer, r_buffer);

  for (int t = 0; t < T; t++) {
    for (int u = 0; u < U; u++) {
      alpha({t, u}) = DTYPE(-INFINITY);
    }
  }
  alpha({0, 0}) = DTYPE(0);

  for (int t = 1; t < T; ++t) { // u == 0.
    if (!check.alphaBlankTransition(t, 0)) {
      break;
    }
    alpha({t, 0}) = alpha({t - 1, 0}) + logProbs({t - 1, 0}).skip();
  }

  for (int u = 1; u < U; ++u) { // t == 0.
    if (!check.alphaEmitTransition(0, u)) {
      break;
    }
    alpha({0, u}) = alpha({0, u - 1}) + logProbs({0, u - 1}).emit();
  }

  for (int u = 1; u < U; ++u) {
    int start_t = 0, end_t = 0;
    check.validTimeRanges(u, start_t, end_t);
    for (int t = start_t; t < end_t + 1; ++t) {
      DTYPE skip = DTYPE(-INFINITY), emit = DTYPE(-INFINITY);

      if (check.alphaBlankTransition(t, u)) {
        skip = alpha({t - 1, u}) + logProbs({t - 1, u}).skip();
      }

      if (check.alphaEmitTransition(t, u)) {
        emit = alpha({t, u - 1}) + logProbs({t, u - 1}).emit();
      }

      if (skip != DTYPE(-INFINITY) || emit != DTYPE(-INFINITY)) {
        alpha({t, u}) = math::lse(skip, emit);
      }
    }
  }
  DTYPE forward_score = alpha({T - 1, U - 1}) + logProbs({T - 1, U - 1}).skip();

  return forward_score;
}

template <typename DTYPE>
DTYPE ComputeBetaOneSequence(
    const Options& options,
    TensorView<const LogProbs<DTYPE>>& logProbs,
    int srcLen,
    int tgtLen,
    TensorView<DTYPE>& beta) {
  const int& T = srcLen;
  const int& U = tgtLen;

  beta({T - 1, U - 1}) = logProbs({T - 1, U - 1}).skip();

  for (int t = T - 2; t >= 0; --t) { // u == U - 1.
    beta({t, U - 1}) = beta({t + 1, U - 1}) + logProbs({t, U - 1}).skip();
  }

  for (int u = U - 2; u >= 0; --u) { // t == T - 1.
    beta({T - 1, u}) = beta({T - 1, u + 1}) + logProbs({T - 1, u}).emit();
  }

  for (int t = T - 2; t >= 0; --t) {
    for (int u = U - 2; u >= 0; --u) {
      beta({t, u}) = math::lse(
          beta({t + 1, u}) + logProbs({t, u}).skip(),
          beta({t, u + 1}) + logProbs({t, u}).emit());
    }
  }

  DTYPE backward_score = beta({0, 0});

  return backward_score;
}

template <typename DTYPE>
DTYPE ComputeBetaOneSequenceRestricted(
    const Options& options,
    TensorView<const LogProbs<DTYPE>>& logProbs,
    int srcLen,
    int tgtLen,
    TensorView<DTYPE>& beta,
    const int* wpEnds = nullptr) {
  const int& T = srcLen;
  const int& U = tgtLen;
  const int& lBuffer = options.lBuffer_;
  const int& r_buffer = options.rBuffer_;
  AlignmentRestrictionCheck check(wpEnds, T, U, lBuffer, r_buffer);

  for (int t = 0; t < T; t++) {
    for (int u = 0; u < U; u++) {
      beta({t, u}) = DTYPE(-INFINITY);
    }
  }

  beta({T - 1, U - 1}) = logProbs({T - 1, U - 1}).skip();

  for (int t = T - 2; t >= 0; --t) { // u == U - 1.
    if (!check.betaBlankTransition(t, U - 1)) {
      break;
    }
    beta({t, U - 1}) = beta({t + 1, U - 1}) + logProbs({t, U - 1}).skip();
  }

  for (int u = U - 2; u >= 0; --u) { // t == T - 1.
    if (!check.betaEmitTransition(T - 1, u)) {
      break;
    }
    beta({T - 1, u}) = beta({T - 1, u + 1}) + logProbs({T - 1, u}).emit();
  }
  for (int u = U - 2; u >= 0; --u) {
    int start_t = 0, end_t = 0;
    check.validTimeRanges(u, start_t, end_t);
    for (int t = end_t; t >= start_t; --t) {
      DTYPE skip = DTYPE(-INFINITY), emit = DTYPE(-INFINITY);
      if (check.betaBlankTransition(t, u)) {
        skip = beta({t + 1, u}) + logProbs({t, u}).skip();
      }

      if (check.betaEmitTransition(t, u)) {
        emit = beta({t, u + 1}) + logProbs({t, u}).emit();
      }

      if (skip != DTYPE(-INFINITY) || emit != DTYPE(-INFINITY)) {
        beta({t, u}) = math::lse(skip, emit);
      }
    }
  }
  DTYPE backward_score = beta({0, 0});

  return backward_score;
}

template <typename DTYPE>
DTYPE ComputeAlphaOrBetaOneSequence(
    int thread,
    const Options& options,
    TensorView<const LogProbs<DTYPE>>& logProbs,
    int srcLen,
    int tgtLen,
    TensorView<DTYPE>& alpha,
    TensorView<DTYPE>& beta,
    const int* wpEnds = nullptr) {
  if (wpEnds == nullptr) {
    if (thread & 1) {
      return ComputeAlphaOneSequence<DTYPE>(
          /*options=*/options,
          /*logProbs=*/logProbs,
          /*srcLen=*/srcLen,
          /*tgtLen=*/tgtLen,
          /*alpha=*/alpha);
    } else {
      return ComputeBetaOneSequence<DTYPE>(
          /*options=*/options,
          /*logProbs=*/logProbs,
          /*srcLen=*/srcLen,
          /*tgtLen=*/tgtLen,
          /*beta=*/beta);
    }
  } else {
    if (thread & 1) {
      return ComputeAlphaOneSequenceRestricted<DTYPE>(
          /*options=*/options,
          /*logProbs=*/logProbs,
          /*srcLen=*/srcLen,
          /*tgtLen=*/tgtLen,
          /*alpha=*/alpha,
          /*wpEnds=*/wpEnds);
    } else {
      return ComputeBetaOneSequenceRestricted<DTYPE>(
          /*options=*/options,
          /*logProbs=*/logProbs,
          /*srcLen=*/srcLen,
          /*tgtLen=*/tgtLen,
          /*beta=*/beta,
          /*wpEnds=*/wpEnds);
    }
  }
}

template <typename DTYPE, typename CAST_DTYPE>
void ComputeAlphasBetas(
    const Options& options,
    const CAST_DTYPE* logProbs,
    const int* srcLengths,
    const int* tgtLengths,
    CAST_DTYPE* alphas,
    CAST_DTYPE* betas,
    DTYPE* costs,
    const int* wpEnds = nullptr) {
  std::vector<TensorView<const LogProbs<CAST_DTYPE>>> seqlogProbs;
  std::vector<TensorView<CAST_DTYPE>> seq_alphas;
  std::vector<TensorView<CAST_DTYPE>> seq_betas;
  std::vector<const int*> seq_wpEnds;

  const int& B = options.batchSize_;
  const int& maxT = options.maxSrcLen_;
  const int& maxU = options.maxTgtLen_;

  for (int b = 0; b < B; ++b) {
    seqlogProbs.push_back(TensorView<const LogProbs<CAST_DTYPE>>(
        {maxT, maxU},
        reinterpret_cast<LogProbs<CAST_DTYPE>*>(
            const_cast<CAST_DTYPE*>(logProbs)) +
            b * maxT * maxU));
    seq_alphas.push_back(
        TensorView<CAST_DTYPE>({maxT, maxU}, alphas + b * maxT * maxU));
    seq_betas.push_back(
        TensorView<CAST_DTYPE>({maxT, maxU}, betas + b * maxT * maxU));

    if (wpEnds != nullptr) {
      seq_wpEnds.push_back(wpEnds + b * (maxU));
    }
  }

  std::vector<CAST_DTYPE> scores(B << 1);
  //#pragma omp parallel for
  for (int t = 0; t < (B << 1); ++t) { // use max 2 * B threads.
    int i = (t >> 1);
    scores[t] = ComputeAlphaOrBetaOneSequence<CAST_DTYPE>(
        /*thread=*/t,
        /*options=*/options,
        /*logProbs=*/seqlogProbs[i],
        /*srcLen=*/srcLengths[i],
        /*tgtLen=*/tgtLengths[i] + 1, // with prepended blank.
        /*alpha=*/seq_alphas[i],
        /*beta=*/seq_betas[i],
        /*wpEnds=*/(wpEnds == nullptr) ? nullptr : seq_wpEnds[i]);
  }
  for (int b = 0; b < B; ++b) {
    costs[b] = -scores[b << 1];
  }
}

template <typename DTYPE, typename CAST_DTYPE>
void ComputeGradientsOneSequence(
    const Options& options,
    TensorView<const DTYPE>& logits,
    const int* targets,
    int srcLen,
    int tgtLen,
    TensorView<const CAST_DTYPE>& denom,
    TensorView<const CAST_DTYPE>& alpha,
    TensorView<const CAST_DTYPE>& beta,
    TensorView<DTYPE>& gradients) {
  // don't set gradients to zero to here as gradients might reuse memory from
  // logits

  const int& T = srcLen;
  const int& U = tgtLen;
  const int& D = options.numTargets_;
  const int& blank = options.blank_;
  const CAST_DTYPE clamp = options.clamp_;

  CAST_DTYPE cost = -beta({0, 0});

  // Note - below gradient is different from numpy_transducer, since we
  // compute log_softmax more efficiently within the loss, to save memory The
  // details of the below implementation / equations can be found in Sec 3.2
  // (function merging) in below paper:
  // https://www.microsoft.com/en-us/research/uploads/prod/2019/10/RNNT.pdf

  for (int t = 0; t < T; ++t) {
    for (int u = 0; u < U; ++u) {
      CAST_DTYPE c = alpha({t, u}) + cost - denom({t, u});
      for (int d = 0; d < D; ++d) {
        CAST_DTYPE g = CAST_DTYPE(logits({t, u, d})) + c;
        if (d == blank && t == T - 1 &&
            u == U - 1) { // last blank transition.
          gradients({t, u, d}) = std::exp(g + beta({t, u})) - std::exp(g);
        } else if (d == blank && t < T - 1) {
          gradients({t, u, d}) =
              std::exp(g + beta({t, u})) - std::exp(g + beta({t + 1, u}));
        } else if (u < U - 1 && d == targets[u]) {
          gradients({t, u, d}) =
              std::exp(g + beta({t, u})) - std::exp(g + beta({t, u + 1}));
        } else {
          gradients({t, u, d}) = std::exp(g + beta({t, u}));
        }

        if (clamp > 0) {
          gradients({t, u, d}) =
              math::min(CAST_DTYPE(gradients({t, u, d})), clamp);
          gradients({t, u, d}) =
              math::max(CAST_DTYPE(gradients({t, u, d})), -clamp);
        }
      }
    }
  }

  // zero out the rest of the gradients, necessary when reusing logits memory
  // check the memory location to see if it's necessary
  if (&gradients({0, 0, 0}) == &logits({0, 0, 0})) {
    const int& maxT = options.maxSrcLen_;
    const int& maxU = options.maxTgtLen_;
    for (int t = T; t < maxT; ++t) {
      for (int u = 0; u < maxU; ++u) {
        for (int d = 0; d < D; ++d) {
          gradients({t, u, d}) = 0.;
        }
      }
    }
    for (int t = 0; t < T; ++t) {
      for (int u = U; u < maxU; ++u) {
        for (int d = 0; d < D; ++d) {
          gradients({t, u, d}) = 0.;
        }
      }
    }
  }
}

template <typename DTYPE, typename CAST_DTYPE>
void ComputeGradients(
    const Options& options,
    const DTYPE* logits,
    const int* targets,
    const int* srcLengths,
    const int* tgtLengths,
    const CAST_DTYPE* denominators,
    const CAST_DTYPE* alphas,
    const CAST_DTYPE* betas,
    DTYPE* gradients) {
  std::vector<TensorView<const DTYPE>> seqLogits;
  std::vector<const int*> seqTargets;
  std::vector<TensorView<const CAST_DTYPE>> seqDenoms;
  std::vector<TensorView<const CAST_DTYPE>> seq_alphas;
  std::vector<TensorView<const CAST_DTYPE>> seq_betas;
  std::vector<TensorView<DTYPE>> seq_gradients;

  const int& B = options.batchSize_;
  const int& maxT = options.maxSrcLen_;
  const int& maxU = options.maxTgtLen_;
  const int& D = options.numTargets_;
  for (int b = 0; b < B; ++b) {
    seqLogits.push_back(
        TensorView<const DTYPE>({maxT, maxU, D}, logits + b * maxT * maxU * D));
    seqTargets.push_back(targets + b * (maxU - 1));
    seqDenoms.push_back(TensorView<const CAST_DTYPE>(
        {maxT, maxU}, denominators + b * maxT * maxU));
    seq_alphas.push_back(
        TensorView<const CAST_DTYPE>({maxT, maxU}, alphas + b * maxT * maxU));
    seq_betas.push_back(
        TensorView<const CAST_DTYPE>({maxT, maxU}, betas + b * maxT * maxU));
    seq_gradients.push_back(
        TensorView<DTYPE>({maxT, maxU, D}, gradients + b * maxT * maxU * D));
  }

  //#pragma omp parallel for
  for (int b = 0; b < B; ++b) { // use max 2 * B threads.
    ComputeGradientsOneSequence<DTYPE, CAST_DTYPE>(
        /*options=*/options,
        /*logits=*/seqLogits[b],
        /*targets=*/seqTargets[b],
        /*srcLen=*/srcLengths[b],
        /*tgtLen=*/tgtLengths[b] + 1, // with prepended blank.
        /*denom=*/seqDenoms[b],
        /*alpha=*/seq_alphas[b],
        /*beta=*/seq_betas[b],
        /*gradients=*/seq_gradients[b]);
  }
}

template <typename DTYPE, typename CAST_DTYPE>
void ComputeAlphas(
    const Options& options,
    const CAST_DTYPE* logProbs,
    const int* srcLengths,
    const int* tgtLengths,
    CAST_DTYPE* alphas,
    const int* wpEnds = nullptr) {
  std::vector<TensorView<const LogProbs<CAST_DTYPE>>> seqlogProbs;
  std::vector<TensorView<CAST_DTYPE>> seq_alphas;
  std::vector<const int*> seq_wpEnds;

  const int& B = options.batchSize_;
  const int& maxT = options.maxSrcLen_;
  const int& maxU = options.maxTgtLen_;

  for (int b = 0; b < B; ++b) {
    seqlogProbs.push_back(TensorView<const LogProbs<CAST_DTYPE>>(
        {maxT, maxU},
        reinterpret_cast<LogProbs<CAST_DTYPE>*>(
            const_cast<CAST_DTYPE*>(logProbs)) +
            b * maxT * maxU));
    seq_alphas.push_back(
        TensorView<CAST_DTYPE>({maxT, maxU}, alphas + b * maxT * maxU));
    if (wpEnds != nullptr) {
      seq_wpEnds.push_back(wpEnds + b * (maxU));
    }
  }

  std::vector<CAST_DTYPE> scores(B << 1);
  //#pragma omp parallel for
  for (int i = 0; i < B; ++i) { // use max 2 * B threads.
    if (wpEnds == nullptr) {
      ComputeAlphaOneSequence<DTYPE>(
          options,
          /*logProbs=*/seqlogProbs[i],
          /*srcLen=*/srcLengths[i],
          /*tgtLen=*/tgtLengths[i] + 1, // with prepended blank.
          /*alpha=*/seq_alphas[i]);
    } else {
      ComputeAlphaOneSequenceRestricted<DTYPE>(
          /*options=*/options,
          /*logProbs=*/seqlogProbs[i],
          /*srcLen=*/srcLengths[i],
          /*tgtLen=*/tgtLengths[i] + 1, // with prepended blank.
          /*alpha=*/seq_alphas[i],
          /*wpEnds=*/seq_wpEnds[i]);
    }
  }
}

template <typename DTYPE, typename CAST_DTYPE>
void ComputeBetas(
    const Options& options,
    const CAST_DTYPE* logProbs,
    const int* srcLengths,
    const int* tgtLengths,
    CAST_DTYPE* costs,
    CAST_DTYPE* betas,
    const int* wpEnds = nullptr) {
  std::vector<TensorView<const LogProbs<CAST_DTYPE>>> seqlogProbs;
  std::vector<TensorView<CAST_DTYPE>> seq_betas;
  std::vector<const int*> seq_wpEnds;

  const int& B = options.batchSize_;
  const int& maxT = options.maxSrcLen_;
  const int& maxU = options.maxTgtLen_;

  for (int b = 0; b < B; ++b) {
    seqlogProbs.push_back(TensorView<const LogProbs<CAST_DTYPE>>(
        {maxT, maxU},
        reinterpret_cast<LogProbs<CAST_DTYPE>*>(
            const_cast<CAST_DTYPE*>(logProbs)) +
            b * maxT * maxU));
    seq_betas.push_back(
        TensorView<CAST_DTYPE>({maxT, maxU}, betas + b * maxT * maxU));
    if (wpEnds != nullptr) {
      seq_wpEnds.push_back(wpEnds + b * (maxU));
    }
  }

  std::vector<CAST_DTYPE> scores(B << 1);
  //#pragma omp parallel for
  for (int i = 0; i < B; ++i) { // use max 2 * B threads.
    if (wpEnds == nullptr) {
      ComputeBetaOneSequence<DTYPE>(
          options,
          /*logProbs=*/seqlogProbs[i],
          /*srcLen=*/srcLengths[i],
          /*tgtLen=*/tgtLengths[i] + 1, // with prepended blank.
          /*betas=*/seq_betas[i]);
    } else {
      ComputeBetaOneSequenceRestricted<DTYPE>(
          /*options=*/options,
          /*logProbs=*/seqlogProbs[i],
          /*srcLen=*/srcLengths[i],
          /*tgtLen=*/tgtLengths[i] + 1, // with prepended blank.
          /*betas=*/seq_betas[i],
          /*wpEnds=*/seq_wpEnds[i]);
    }
  }
}

} // namespace cpu
} // namespace rnnt
} // namespace torchaudio