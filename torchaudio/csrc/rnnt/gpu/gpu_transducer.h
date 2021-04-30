#pragma once

#ifdef USE_CUDA

#include <torchaudio/csrc/rnnt/gpu/gpu_kernel_utils.cuh>
#include <torchaudio/csrc/rnnt/gpu/gpu_kernels.cuh>
#include <torchaudio/csrc/rnnt/gpu/gpu_alignment_restrictions.cuh>
#include <torchaudio/csrc/rnnt/workspace.h>

namespace torchaudio {
namespace rnnt {
namespace gpu {

#define gpuErrchk(ans) \
  { gpuAssert((ans), __FILE__, __LINE__); }

inline void
gpuAssert(cudaError_t code, const char* file, int line, bool abort = true) {
  if (code != cudaSuccess) {
    fprintf(
        stderr,
        "\nGPUassert: %s %s %d\n",
        cudaGetErrorString(code),
        file,
        line);
    if (abort)
      exit(code);
  }
}

template <typename DTYPE, typename CAST_DTYPE>
status_t LogSumExp2D(
    cudaStream_t stream,
    int N,
    int D,
    const DTYPE* logits, // [N, D]
    CAST_DTYPE* outputs) {
  { // compute max among D.
    dim3 block_dims(N);
    dim3 thread_dims(REDUCE_THREADS);

    ReduceMax2D<REDUCE_THREADS, DTYPE, CAST_DTYPE>
        <<<block_dims, thread_dims, 0, stream>>>(
            /*dim=*/D,
            /*inputs=*/logits,
            /*outputs=*/outputs);

    // BUGBUG: These error codes are only accurate when launching with
    // blocking. Otherwise they usually reflect earlier errors.
    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_DENOMINATOR_REDUCE_MAX_FAILED;
    }
  }

  { // compute log(sum(exp(d_i - max)))
    dim3 block_dims(N);
    dim3 thread_dims(REDUCE_THREADS);

    ReduceLogSumExpGivenMax2D<REDUCE_THREADS, DTYPE, CAST_DTYPE>
        <<<block_dims, thread_dims, 0, stream>>>(
            /*dim=*/D,
            /*inputs=*/logits,
            /*outputs=*/outputs);

    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_DENOMINATOR_REDUCE_SUM_FAILED;
    }
  }

  return SUCCESS;
}

// Inputs:
//   workspace: workspace.
//   logits: pointer to (B, max_T, max_U, D) logits.
//   targets: pointer to (B, max_U - 1) targets in the batch.
//   srcLengths: pointer to (B, ) source lengths in the batch.
//   tgtLengths: pointer to (B, ) target lengths in the batch.
//
// Outputs:
//   costs: pointer to (B, ) costs in the batch.
//   gradients: pointer to (B, max_T, max_U, D) gradients in the batch.
template <typename DTYPE, typename CAST_DTYPE>
status_t Compute(
    const Workspace<CAST_DTYPE>& workspace,
    const DTYPE* logits,
    const int* targets,
    const int* srcLengths,
    const int* tgtLengths,
    DTYPE* costs,
    DTYPE* gradients = nullptr,
    const int* wpEnds = nullptr) {
  const Options& options = workspace.GetOptions();

  const cudaStream_t& stream = options.stream_;
  const int& B = options.batchSize_;
  const int& H = options.nHypos_;
  const int& max_T = options.maxSrcLen_;
  const int& max_U = options.maxTgtLen_;
  const int& D = options.numTargets_;
  const int& blank = options.blank_;
  const CAST_DTYPE clamp = options.clamp_;

  const int& l_buffer = options.lBuffer_;
  const int& r_buffer = options.rBuffer_;
  const bool& fusedLogSmax = options.fusedLogSmax_;

  { // compute denominators.
    status_t status = LogSumExp2D<DTYPE, CAST_DTYPE>(
        /*stream=*/stream,
        /*N=*/B * H * max_T * max_U,
        /*D=*/D,
        /*logits=*/logits,
        /*denominators=*/workspace.GetPointerToDenominators());

    if (status != SUCCESS) {
      return status;
    }
  }

  { // compute log probability pairs (blank and target).
    int num_segments =
        (max_T + MAX_THREADS_PER_BLOCK - 1) / MAX_THREADS_PER_BLOCK;
    dim3 block_dims(num_segments, max_U, B * H);
    dim3 thread_dims(MAX_THREADS_PER_BLOCK);

    ComputeLogProbs<DTYPE, CAST_DTYPE><<<block_dims, thread_dims, 0, stream>>>(
        /*max_src_len=*/max_T,
        /*max_tgt_len=*/max_U,
        /*num_targets=*/D,
        /*blank=*/blank,
        /*logits=*/logits,
        /*targets=*/targets,
        /*srcLengths=*/srcLengths,
        /*tgtLengths=*/tgtLengths,
        /*denominators=*/workspace.GetPointerToDenominators(),
        /*log_probs=*/workspace.GetPointerToLogProbs(),
        H,
        fusedLogSmax);

    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_LOG_PROBS_FAILED;
    }
  }

  { // compute alphas, betas and costs.
    // warp is usually a group of threads (32)
    int num_warps = (max_T + WARP_SIZE - 1) / WARP_SIZE;

    // each block is identified by 3 d tuple.
    // we are using num_warp * max_U * B * H blocks
    // where num_warp is division among Time axis
    dim3 block_dims(num_warps, max_U, B * H);

    // each thread is identified by a 2 d tuple
    // 2nd dim is 2. 1 for alpha, 1 for beta
    dim3 thread_dims(WARP_SIZE, 2);

    ComputeAlphasBetasCosts<DTYPE, CAST_DTYPE>
        <<<block_dims, thread_dims, 0, stream>>>(
            /*max_src_len=*/max_T,
            /*max_tgt_len=*/max_U,
            /*num_targets=*/D,
            /*blank=*/blank,
            /*log_probs=*/workspace.GetPointerToLogProbs(),
            /*srcLengths=*/srcLengths,
            /*tgtLengths=*/tgtLengths,
            /*alpha_counters=*/workspace.GetPointerToAlphaCounters(),
            /*alphas=*/workspace.GetPointerToAlphas(),
            /*beta_counters=*/workspace.GetPointerToBetaCounters(),
            /*betas=*/workspace.GetPointerToBetas(),
            /*costs=*/costs,
            /*wpEnds=*/wpEnds,
            /*l_buffer=*/l_buffer,
            /*r_buffer=*/r_buffer,
            /*warp_size=*/WARP_SIZE,
            /*num_warps=*/num_warps,
            /*sparse=*/false,
            /*validRanges=*/nullptr,
            /*cellsPerSample=*/nullptr,
            H);
    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_ALPHAS_BETAS_COSTS_FAILED;
    }
  }

  if (gradients != nullptr) { // compute gradients.
    // don't set gradients to zero to here as gradients might reuse memory from
    // logits

    int num_blocks =
        (max_T + MAX_THREADS_PER_BLOCK - 1) / MAX_THREADS_PER_BLOCK;
    dim3 block_dims(num_blocks, max_U, B * H);
    dim3 thread_dims(MAX_THREADS_PER_BLOCK);

    ComputeGradients<DTYPE, CAST_DTYPE><<<block_dims, thread_dims, 0, stream>>>(
        /*max_src_len=*/max_T,
        /*max_tgt_len=*/max_U,
        /*num_targets=*/D,
        /*blank=*/blank,
        /*clamp=*/clamp,
        /*logits=*/logits,
        /*targets=*/targets,
        /*srcLengths=*/srcLengths,
        /*tgtLengths=*/tgtLengths,
        /*denominators=*/workspace.GetPointerToDenominators(),
        /*alphas=*/workspace.GetPointerToAlphas(),
        /*betas=*/workspace.GetPointerToBetas(),
        /*gradients=*/gradients,
        /*sparse=*/false,
        /*validRanges=*/nullptr,
        /*cellsPerSample=*/nullptr,
        H,
        fusedLogSmax);
    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_GRADIENTS_FAILED;
    }
  }

  return SUCCESS;
}

// Inputs:
//   workspace: workspace.
//   logits: pointer to (sparseCells, D) logits.
//   targets: pointer to (B, max_U - 1) targets in the batch.
//   srcLengths: pointer to (B, ) source lengths in the batch.
//   tgtLengths: pointer to (B, ) target lengths in the batch.
//
// Outputs:
//   costs: pointer to (B, ) costs in the batch.
//   gradients: pointer to (sparseCells, D) gradients in the batch.
template <typename DTYPE, typename CAST_DTYPE>
status_t ComputeSparse(
    const Workspace<CAST_DTYPE>& workspace,
    const DTYPE* logits,
    const int* targets,
    const int* srcLengths,
    const int* tgtLengths,
    DTYPE* costs,
    DTYPE* gradients = nullptr,
    const int* wpEnds = nullptr,
    const int* validRanges = nullptr,
    const int* cellsPerSample = nullptr) {
  const Options& options = workspace.GetOptions();

  const cudaStream_t& stream = options.stream_;
  const int& B = options.batchSize_;
  const int& H = options.nHypos_;
  const int& max_T = options.maxSrcLen_;
  const int& max_U = options.maxTgtLen_;
  const int& D = options.numTargets_;
  const int& blank = options.blank_;
  const CAST_DTYPE clamp = options.clamp_;

  const int& l_buffer = options.lBuffer_;
  const int& r_buffer = options.rBuffer_;
  const bool& fusedLogSmax = options.fusedLogSmax_;

  const int& sparseCells = options.sparseCells_;
  { // compute denominators.
    // for sparse kernel, we call kernel with sparseSize
    status_t status = LogSumExp2D<DTYPE, CAST_DTYPE>(
        /*stream=*/stream,
        /*N=*/sparseCells, // not B * max_T * max_U
        /*D=*/D,
        /*logits=*/logits,
        /*denominators=*/workspace.GetPointerToDenominators());
    gpuErrchk(cudaGetLastError());
    if (status != SUCCESS) {
      return status;
    }
  }
  { // compute log probability pairs (blank and target).
    int num_segments =
        (max_T + MAX_THREADS_PER_BLOCK - 1) / MAX_THREADS_PER_BLOCK;
    dim3 block_dims(num_segments, max_U, B * H);
    dim3 thread_dims(MAX_THREADS_PER_BLOCK);

    ComputeLogProbsSparse<DTYPE, CAST_DTYPE>
        <<<block_dims, thread_dims, 0, stream>>>(
            max_T,
            max_U,
            /*num_targets=*/D,
            /*blank=*/blank,
            /*logits=*/logits,
            /*targets=*/targets,
            /*srcLengths=*/srcLengths,
            /*tgtLengths=*/tgtLengths,
            /*denominators=*/workspace.GetPointerToDenominators(),
            /*log_probs=*/workspace.GetPointerToLogProbs(),
            wpEnds,
            validRanges,
            cellsPerSample,
            H,
            fusedLogSmax);
    gpuErrchk(cudaGetLastError());
  }
  { // compute alphas, betas and costs.
    // warp is usually a group of threads (32)
    int num_warps = (max_T + WARP_SIZE - 1) / WARP_SIZE;

    // each block is identified by 3 d tuple.
    // we are using num_warp * max_U * B * H blocks
    // where num_warp is division among Time axis
    dim3 block_dims(num_warps, max_U, B * H);

    // each thread is identified by a 2 d tuple
    // 2nd dim is 2. 1 for alpha, 1 for beta
    dim3 thread_dims(WARP_SIZE, 2);

    ComputeAlphasBetasCosts<DTYPE, CAST_DTYPE>
        <<<block_dims, thread_dims, 0, stream>>>(
            /*max_src_len=*/max_T,
            /*max_tgt_len=*/max_U,
            /*num_targets=*/D,
            /*blank=*/blank,
            /*log_probs=*/workspace.GetPointerToLogProbs(),
            /*srcLengths=*/srcLengths,
            /*tgtLengths=*/tgtLengths,
            /*alpha_counters=*/workspace.GetPointerToAlphaCounters(),
            /*alphas=*/workspace.GetPointerToAlphas(),
            /*beta_counters=*/workspace.GetPointerToBetaCounters(),
            /*betas=*/workspace.GetPointerToBetas(),
            /*costs=*/costs,
            /*wpEnds=*/wpEnds,
            /*l_buffer=*/l_buffer,
            /*r_buffer=*/r_buffer,
            /*warp_size=*/WARP_SIZE,
            /*num_warps=*/num_warps,
            true,
            validRanges,
            cellsPerSample,
            H);
    gpuErrchk(cudaGetLastError());
  }

  if (gradients != nullptr) { // compute gradients.
    // don't zero gradients here as gradient might share memory with logits

    int num_blocks =
        (max_T + MAX_THREADS_PER_BLOCK - 1) / MAX_THREADS_PER_BLOCK;
    dim3 block_dims(num_blocks, max_U, B * H);
    dim3 thread_dims(MAX_THREADS_PER_BLOCK);

    ComputeGradients<DTYPE, CAST_DTYPE><<<block_dims, thread_dims, 0, stream>>>(
        /*max_src_len=*/max_T,
        /*max_tgt_len=*/max_U,
        /*num_targets=*/D,
        /*blank=*/blank,
        /*clamp=*/clamp,
        /*logits=*/logits,
        /*targets=*/targets,
        /*srcLengths=*/srcLengths,
        /*tgtLengths=*/tgtLengths,
        /*denominators=*/workspace.GetPointerToDenominators(),
        /*alphas=*/workspace.GetPointerToAlphas(),
        /*betas=*/workspace.GetPointerToBetas(),
        /*gradients=*/gradients,
        /*sparse=*/true,
        /*validRanges=*/validRanges,
        /*cellsPerSample=*/cellsPerSample,
        H,
        fusedLogSmax);
    gpuErrchk(cudaGetLastError());
  }

  return SUCCESS;
}

template <typename DTYPE, typename CAST_DTYPE>
status_t ComputeAlphas(
    const Workspace<CAST_DTYPE>& workspace,
    const DTYPE* logits,
    const int* targets,
    const int* srcLengths,
    const int* tgtLengths,
    DTYPE* alphas,
    const int* wpEnds = nullptr) {
  const Options& options = workspace.GetOptions();

  const cudaStream_t& stream = options.stream_;
  const int& B = options.batchSize_;
  const int& H = options.nHypos_;
  const int& max_T = options.maxSrcLen_;
  const int& max_U = options.maxTgtLen_;
  const int& D = options.numTargets_;
  const int& blank = options.blank_;

  const int& l_buffer = options.lBuffer_;
  const int& r_buffer = options.rBuffer_;

  { // compute denominators.
    status_t status = LogSumExp2D<DTYPE, CAST_DTYPE>(
        /*stream=*/stream,
        /*N=*/B * H * max_T * max_U,
        /*D=*/D,
        /*logits=*/logits,
        /*denominators=*/workspace.GetPointerToDenominators());

    if (status != SUCCESS) {
      return status;
    }
  }

  { // compute log probability pairs (blank and target).
    int num_segments =
        (max_T + MAX_THREADS_PER_BLOCK - 1) / MAX_THREADS_PER_BLOCK;
    dim3 block_dims(num_segments, max_U, B * H);
    dim3 thread_dims(MAX_THREADS_PER_BLOCK);

    ComputeLogProbs<DTYPE, CAST_DTYPE><<<block_dims, thread_dims, 0, stream>>>(
        /*max_src_len=*/max_T,
        /*max_tgt_len=*/max_U,
        /*num_targets=*/D,
        /*blank=*/blank,
        /*logits=*/logits,
        /*targets=*/targets,
        /*srcLengths=*/srcLengths,
        /*tgtLengths=*/tgtLengths,
        /*denominators=*/workspace.GetPointerToDenominators(),
        /*log_probs=*/workspace.GetPointerToLogProbs(),
        H);

    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_LOG_PROBS_FAILED;
    }
  }
  { // compute alphas
    // warp is usually a group of threads (32)
    int num_warps = (max_T + WARP_SIZE - 1) / WARP_SIZE;

    // each block is identified by 3 d tuple.
    // we are using num_warp * max_U * B blocks
    // where num_warp is division among Time axis
    dim3 block_dims(num_warps, max_U, B * H);

    // each thread is identified by a 2 d tuple
    // 2nd dim is 1 for alpha only
    dim3 thread_dims(WARP_SIZE, 1);

    if (wpEnds == nullptr) {
      ComputeAlphasWrapper<DTYPE, CAST_DTYPE>
          <<<block_dims, thread_dims, 0, stream>>>(
              /*max_src_len=*/max_T,
              /*max_tgt_len=*/max_U,
              /*num_targets=*/D,
              /*blank=*/blank,
              /*log_probs=*/workspace.GetPointerToLogProbs(),
              /*srcLengths=*/srcLengths,
              /*tgtLengths=*/tgtLengths,
              /*alpha_counters=*/workspace.GetPointerToAlphaCounters(),
              /*alphas=*/(volatile DTYPE*)alphas,
              H);
    } else {
      // cudaMemset(alphas, 0, B * max_T * max_U * sizeof(DTYPE));
      ComputeAlphasRestrictedWrapper<DTYPE, CAST_DTYPE>
          <<<block_dims, thread_dims, 0, stream>>>(
              max_T,
              max_U,
              D,
              blank,
              workspace.GetPointerToLogProbs(),
              srcLengths,
              tgtLengths,
              workspace.GetPointerToAlphaCounters(),
              // workspace.GetPointerToAlphas(),
              (volatile DTYPE*)alphas,
              wpEnds,
              l_buffer,
              r_buffer,
              WARP_SIZE,
              false,
              nullptr,
              nullptr,
              H);
    }
    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_ALPHAS_BETAS_COSTS_FAILED;
    }
  }

  return SUCCESS;
}

template <typename DTYPE, typename CAST_DTYPE>
status_t ComputeAlphasSparse(
    const Workspace<CAST_DTYPE>& workspace,
    const DTYPE* logits,
    const int* targets,
    const int* srcLengths,
    const int* tgtLengths,
    DTYPE* alphas,
    const int* wpEnds = nullptr,
    const int* validRanges = nullptr,
    const int* cellsPerSample = nullptr) {
  const Options& options = workspace.GetOptions();
  const cudaStream_t& stream = options.stream_;
  const int& B = options.batchSize_;
  const int& H = options.nHypos_;
  const int& max_T = options.maxSrcLen_;
  const int& max_U = options.maxTgtLen_;
  const int& D = options.numTargets_;
  const int& blank = options.blank_;

  const int& l_buffer = options.lBuffer_;
  const int& r_buffer = options.rBuffer_;

  const int& sparseCells = options.sparseCells_;

  { // compute denominators.
    // for sparse kernel, we would call kernel with sparseSize
    status_t status = LogSumExp2D<DTYPE, CAST_DTYPE>(
        /*stream=*/stream,
        /*N=*/sparseCells, // not B * max_T * max_U
        /*D=*/D,
        /*logits=*/logits,
        /*denominators=*/workspace.GetPointerToDenominators());

    if (status != SUCCESS) {
      return status;
    }
  }

  { // compute log probability pairs (blank and target).
    int num_segments =
        (max_T + MAX_THREADS_PER_BLOCK - 1) / MAX_THREADS_PER_BLOCK;
    dim3 block_dims(num_segments, max_U, B * H);
    dim3 thread_dims(MAX_THREADS_PER_BLOCK);

    ComputeLogProbsSparse<DTYPE, CAST_DTYPE>
        <<<block_dims, thread_dims, 0, stream>>>(
            max_T,
            max_U,
            /*num_targets=*/D,
            /*blank=*/blank,
            /*logits=*/logits,
            /*targets=*/targets,
            /*srcLengths=*/srcLengths,
            /*tgtLengths=*/tgtLengths,
            /*denominators=*/workspace.GetPointerToDenominators(),
            /*log_probs=*/workspace.GetPointerToLogProbs(),
            wpEnds,
            validRanges,
            cellsPerSample,
            H);

    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_LOG_PROBS_FAILED;
    }
  }
  { // compute alphas
    // warp is usually a group of threads (32)
    int num_warps = (max_T + WARP_SIZE - 1) / WARP_SIZE;

    // each block is identified by 3 d tuple.
    // we are using num_warp * max_U * B blocks
    // where num_warp is division among Time axis
    dim3 block_dims(num_warps, max_U, B * H);

    // each thread is identified by a 2 d tuple
    // 2nd dim is 1 for alpha only
    dim3 thread_dims(WARP_SIZE, 1);

    // cudaMemset(alphas, 0, B * max_T * max_U * sizeof(DTYPE));
    ComputeAlphasRestrictedWrapper<DTYPE, CAST_DTYPE>
        <<<block_dims, thread_dims, 0, stream>>>(
            max_T,
            max_U,
            D,
            blank,
            workspace.GetPointerToLogProbs(),
            srcLengths,
            tgtLengths,
            workspace.GetPointerToAlphaCounters(),
            // workspace.GetPointerToAlphas(),
            (volatile DTYPE*)alphas,
            wpEnds,
            l_buffer,
            r_buffer,
            WARP_SIZE,
            true,
            validRanges,
            cellsPerSample,
            H);

    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_ALPHAS_BETAS_COSTS_FAILED;
    }
  }

  return SUCCESS;
}

template <typename DTYPE, typename CAST_DTYPE>
status_t ComputeBetas(
    const Workspace<CAST_DTYPE>& workspace,
    const DTYPE* logits,
    const int* targets,
    const int* srcLengths,
    const int* tgtLengths,
    DTYPE* costs,
    DTYPE* betas,
    const int* wpEnds = nullptr) {
  const Options& options = workspace.GetOptions();

  const cudaStream_t& stream = options.stream_;
  const int& B = options.batchSize_;
  const int& H = options.nHypos_;
  const int& max_T = options.maxSrcLen_;
  const int& max_U = options.maxTgtLen_;
  const int& D = options.numTargets_;
  const int& blank = options.blank_;

  const int& l_buffer = options.lBuffer_;
  const int& r_buffer = options.rBuffer_;

  { // compute denominators.
    status_t status = LogSumExp2D<DTYPE, CAST_DTYPE>(
        /*stream=*/stream,
        /*N=*/B * H * max_T * max_U,
        /*D=*/D,
        /*logits=*/logits,
        /*denominators=*/workspace.GetPointerToDenominators());

    if (status != SUCCESS) {
      return status;
    }
  }

  { // compute log probability pairs (blank and target).
    int num_segments =
        (max_T + MAX_THREADS_PER_BLOCK - 1) / MAX_THREADS_PER_BLOCK;
    dim3 block_dims(num_segments, max_U, B * H);
    dim3 thread_dims(MAX_THREADS_PER_BLOCK);

    ComputeLogProbs<DTYPE, CAST_DTYPE><<<block_dims, thread_dims, 0, stream>>>(
        /*max_src_len=*/max_T,
        /*max_tgt_len=*/max_U,
        /*num_targets=*/D,
        /*blank=*/blank,
        /*logits=*/logits,
        /*targets=*/targets,
        /*srcLengths=*/srcLengths,
        /*tgtLengths=*/tgtLengths,
        /*denominators=*/workspace.GetPointerToDenominators(),
        /*log_probs=*/workspace.GetPointerToLogProbs(),
        H);

    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_LOG_PROBS_FAILED;
    }
  }
  { // compute betas
    // warp is usually a group of threads (32)
    int num_warps = (max_T + WARP_SIZE - 1) / WARP_SIZE;

    // each block is identified by 3 d tuple.
    // we are using num_warp * max_U * B blocks
    // where num_warp is division among Time axis
    dim3 block_dims(num_warps, max_U, B * H);

    // each thread is identified by a 2 d tuple
    // 2nd dim is 1 for betas only
    dim3 thread_dims(WARP_SIZE, 1);

    if (wpEnds == nullptr) {
      ComputeBetasWrapper<DTYPE, CAST_DTYPE>
          <<<block_dims, thread_dims, 0, stream>>>(
              /*max_src_len=*/max_T,
              /*max_tgt_len=*/max_U,
              /*num_targets=*/D,
              /*blank=*/blank,
              /*log_probs=*/workspace.GetPointerToLogProbs(),
              /*srcLengths=*/srcLengths,
              /*tgtLengths=*/tgtLengths,
              /*alpha_counters=*/workspace.GetPointerToBetaCounters(),
              /*alphas=*/(volatile DTYPE*)betas,
              costs,
              H);
    } else {
      ComputeBetasCostsRestrictedWrapper<DTYPE, CAST_DTYPE>
          <<<block_dims, thread_dims, 0, stream>>>(
              max_T,
              max_U,
              D,
              blank,
              workspace.GetPointerToLogProbs(),
              srcLengths,
              tgtLengths,
              workspace.GetPointerToBetaCounters(),
              costs,
              (volatile DTYPE*)betas,
              wpEnds,
              l_buffer,
              r_buffer,
              WARP_SIZE,
              num_warps,
              false,
              nullptr,
              nullptr,
              H);
      cudaDeviceSynchronize();
    }
    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_ALPHAS_BETAS_COSTS_FAILED;
    }
  }

  return SUCCESS;
}

template <typename DTYPE, typename CAST_DTYPE>
status_t ComputeBetasSparse(
    const Workspace<CAST_DTYPE>& workspace,
    const DTYPE* logits,
    const int* targets,
    const int* srcLengths,
    const int* tgtLengths,
    DTYPE* costs,
    DTYPE* betas,
    const int* wpEnds = nullptr,
    const int* validRanges = nullptr,
    const int* cellsPerSample = nullptr) {
  const Options& options = workspace.GetOptions();
  const cudaStream_t& stream = options.stream_;
  const int& B = options.batchSize_;
  const int& H = options.nHypos_;
  const int& max_T = options.maxSrcLen_;
  const int& max_U = options.maxTgtLen_;
  const int& D = options.numTargets_;
  const int& blank = options.blank_;

  const int& l_buffer = options.lBuffer_;
  const int& r_buffer = options.rBuffer_;

  const int& sparseCells = options.sparseCells_;

  { // compute denominators.
    // for sparse kernel, we would call kernel with sparseSize
    status_t status = LogSumExp2D<DTYPE, CAST_DTYPE>(
        /*stream=*/stream,
        /*N=*/sparseCells, // not B * max_T * max_U
        /*D=*/D,
        /*logits=*/logits,
        /*denominators=*/workspace.GetPointerToDenominators());

    if (status != SUCCESS) {
      return status;
    }
  }

  { // compute log probability pairs (blank and target).
    int num_segments =
        (max_T + MAX_THREADS_PER_BLOCK - 1) / MAX_THREADS_PER_BLOCK;
    dim3 block_dims(num_segments, max_U, B * H);
    dim3 thread_dims(MAX_THREADS_PER_BLOCK);

    ComputeLogProbsSparse<DTYPE, CAST_DTYPE>
        <<<block_dims, thread_dims, 0, stream>>>(
            max_T,
            max_U,
            /*num_targets=*/D,
            /*blank=*/blank,
            /*logits=*/logits,
            /*targets=*/targets,
            /*srcLengths=*/srcLengths,
            /*tgtLengths=*/tgtLengths,
            /*denominators=*/workspace.GetPointerToDenominators(),
            /*log_probs=*/workspace.GetPointerToLogProbs(),
            wpEnds,
            validRanges,
            cellsPerSample,
            H);

    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_LOG_PROBS_FAILED;
    }
  }
  { // compute alphas
    // warp is usually a group of threads (32)
    int num_warps = (max_T + WARP_SIZE - 1) / WARP_SIZE;

    // each block is identified by 3 d tuple.
    // we are using num_warp * max_U * B blocks
    // where num_warp is division among Time axis
    dim3 block_dims(num_warps, max_U, B * H);

    // each thread is identified by a 2 d tuple
    // 2nd dim is 1 for alpha only
    dim3 thread_dims(WARP_SIZE, 1);

    // cudaMemset(alphas, 0, B * max_T * max_U * sizeof(DTYPE));
    ComputeBetasCostsRestrictedWrapper<DTYPE, CAST_DTYPE>
        <<<block_dims, thread_dims, 0, stream>>>(
            max_T,
            max_U,
            D,
            blank,
            workspace.GetPointerToLogProbs(),
            srcLengths,
            tgtLengths,
            workspace.GetPointerToBetaCounters(),
            costs,
            (volatile DTYPE*)betas,
            wpEnds,
            l_buffer,
            r_buffer,
            WARP_SIZE,
            num_warps,
            true,
            validRanges,
            cellsPerSample,
            H);
    cudaDeviceSynchronize();

    if (cudaGetLastError() != cudaSuccess) {
      return COMPUTE_ALPHAS_BETAS_COSTS_FAILED;
    }
  }

  return SUCCESS;
}

} // namespace gpu
} // namespace rnnt
} // namespace torchaudio

#endif // USE_CUDA