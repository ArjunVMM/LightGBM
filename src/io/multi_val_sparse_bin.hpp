/*!
 * Copyright (c) 2020 Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License. See LICENSE file in the project root for
 * license information.
 */
#ifndef LIGHTGBM_IO_MULTI_VAL_SPARSE_BIN_HPP_
#define LIGHTGBM_IO_MULTI_VAL_SPARSE_BIN_HPP_

#include <LightGBM/bin.h>
#include <LightGBM/utils/openmp_wrapper.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace LightGBM {

template <typename VAL_T>
class MultiValSparseBin : public MultiValBin {
 public:
  explicit MultiValSparseBin(data_size_t num_data, int num_bin,
                             double estimate_element_per_row)
      : num_data_(num_data),
        num_bin_(num_bin),
        estimate_element_per_row_(estimate_element_per_row) {
    row_ptr_.resize(num_data_ + 1, 0);
    data_size_t estimate_num_data =
        static_cast<data_size_t>(num_data_ * estimate_element_per_row_ * 1.1);
    int num_threads = 1;
#pragma omp parallel
#pragma omp master
    { num_threads = omp_get_num_threads(); }
    if (num_threads > 1) {
      t_data_.resize(num_threads - 1);
      for (size_t i = 0; i < t_data_.size(); ++i) {
        t_data_[i].resize(estimate_num_data / num_threads);
      }
    }
    t_size_.resize(num_threads, 0);
    data_.resize(estimate_num_data / num_threads);
  }

  ~MultiValSparseBin() {}

  data_size_t num_data() const override { return num_data_; }

  int num_bin() const override { return num_bin_; }

  void PushOneRow(int tid, data_size_t idx,
                  const std::vector<uint32_t>& values) override {
    const int pre_alloc_size = 50;
    row_ptr_[idx + 1] = static_cast<data_size_t>(values.size());
    if (tid == 0) {
      if (t_size_[tid] + row_ptr_[idx + 1] > static_cast<data_size_t>(data_.size())) {
        data_.resize(t_size_[tid] + row_ptr_[idx + 1] * pre_alloc_size);
      }
      for (auto val : values) {
        data_[t_size_[tid]++] = static_cast<VAL_T>(val);
      }
    } else {
      if (t_size_[tid] + row_ptr_[idx + 1] > static_cast<data_size_t>(t_data_[tid - 1].size())) {
        t_data_[tid - 1].resize(t_size_[tid] +
                                row_ptr_[idx + 1] * pre_alloc_size);
      }
      for (auto val : values) {
        t_data_[tid - 1][t_size_[tid]++] = static_cast<VAL_T>(val);
      }
    }
  }

  void MergeData(const data_size_t* sizes) {
    Common::FunctionTimer fun_time("MultiValSparseBin::MergeData", global_timer);
    for (data_size_t i = 0; i < num_data_; ++i) {
      row_ptr_[i + 1] += row_ptr_[i];
    }
    if (t_data_.size() > 0) {
      std::vector<data_size_t> offsets(1 + t_data_.size());
      offsets[0] = sizes[0];
      for (size_t tid = 0; tid < t_data_.size() - 1; ++tid) {
        offsets[tid + 1] = offsets[tid] + sizes[tid + 1];
      }
      data_.resize(row_ptr_[num_data_]);
#pragma omp parallel for schedule(static)
      for (int tid = 0; tid < static_cast<int>(t_data_.size()); ++tid) {
        std::copy_n(t_data_[tid].data(), sizes[tid + 1],
                    data_.data() + offsets[tid]);
      }
    } else {
      data_.resize(row_ptr_[num_data_]);
    }
  }

  void FinishLoad() override {
    MergeData(t_size_.data());
    t_size_.clear();
    row_ptr_.shrink_to_fit();
    data_.shrink_to_fit();
    t_data_.clear();
    t_data_.shrink_to_fit();
    // update estimate_element_per_row_ by all data
    estimate_element_per_row_ =
        static_cast<double>(row_ptr_[num_data_]) / num_data_;
  }

  bool IsSparse() override { return true; }

  void ReSize(data_size_t num_data) override {
    if (num_data_ != num_data) {
      num_data_ = num_data;
    }
  }

#define ACC_GH(hist, i, g, h)               \
  const auto ti = static_cast<int>(i) << 1; \
  hist[ti] += g;                            \
  hist[ti + 1] += h;

  template <bool use_indices, bool use_prefetch, bool use_hessians>
  void ConstructHistogramInner(const data_size_t* data_indices,
                               data_size_t start, data_size_t end,
                               const score_t* gradients,
                               const score_t* hessians, hist_t* out) const {
    data_size_t i = start;
    if (use_prefetch) {
      const data_size_t pf_offset = 32 / sizeof(VAL_T);
      const data_size_t pf_end = end - pf_offset;

      for (; i < pf_end; ++i) {
        const auto idx = use_indices ? data_indices[i] : i;
        const auto pf_idx =
            use_indices ? data_indices[i + pf_offset] : i + pf_offset;
        PREFETCH_T0(gradients + pf_idx);
        if (use_hessians) {
          PREFETCH_T0(hessians + pf_idx);
        }
        PREFETCH_T0(row_ptr_.data() + pf_idx);
        PREFETCH_T0(data_.data() + row_ptr_[pf_idx]);
        const auto j_start = RowPtr(idx);
        const auto j_end = RowPtr(idx + 1);
        for (auto j = j_start; j < j_end; ++j) {
          const VAL_T bin = data_[j];
          if (use_hessians) {
            ACC_GH(out, bin, gradients[idx], hessians[idx]);
          } else {
            ACC_GH(out, bin, gradients[idx], 1.0f);
          }
        }
      }
    }
    for (; i < end; ++i) {
      const auto idx = use_indices ? data_indices[i] : i;
      const auto j_start = RowPtr(idx);
      const auto j_end = RowPtr(idx + 1);
      for (auto j = j_start; j < j_end; ++j) {
        const VAL_T bin = data_[j];
        if (use_hessians) {
          ACC_GH(out, bin, gradients[idx], hessians[idx]);
        } else {
          ACC_GH(out, bin, gradients[idx], 1.0f);
        }
      }
    }
  }
#undef ACC_GH

  void ConstructHistogram(const data_size_t* data_indices, data_size_t start,
                          data_size_t end, const score_t* gradients,
                          const score_t* hessians, hist_t* out) const override {
    ConstructHistogramInner<true, true, true>(data_indices, start, end,
                                              gradients, hessians, out);
  }

  void ConstructHistogram(data_size_t start, data_size_t end,
                          const score_t* gradients, const score_t* hessians,
                          hist_t* out) const override {
    ConstructHistogramInner<false, false, true>(nullptr, start, end, gradients,
                                                hessians, out);
  }

  void ConstructHistogram(const data_size_t* data_indices, data_size_t start,
                          data_size_t end, const score_t* gradients,
                          hist_t* out) const override {
    ConstructHistogramInner<true, true, false>(data_indices, start, end,
                                               gradients, nullptr, out);
  }

  void ConstructHistogram(data_size_t start, data_size_t end,
                          const score_t* gradients,
                          hist_t* out) const override {
    ConstructHistogramInner<false, false, false>(nullptr, start, end, gradients,
                                                 nullptr, out);
  }

  void CopySubset(const Bin* full_bin, const data_size_t* used_indices,
                  data_size_t num_used_indices) override {
    auto other_bin = dynamic_cast<const MultiValSparseBin<VAL_T>*>(full_bin);
    row_ptr_.resize(num_data_ + 1, 0);
    data_size_t estimate_num_data =
        static_cast<data_size_t>(num_data_ * estimate_element_per_row_ * 1.5);
    data_.clear();
    data_.reserve(estimate_num_data);
    for (data_size_t i = 0; i < num_used_indices; ++i) {
      for (data_size_t j = other_bin->row_ptr_[used_indices[i]];
           j < other_bin->row_ptr_[used_indices[i] + 1]; ++j) {
        data_.push_back(other_bin->data_[j]);
      }
      row_ptr_[i + 1] = row_ptr_[i] + other_bin->row_ptr_[used_indices[i] + 1] -
                        other_bin->row_ptr_[used_indices[i]];
    }
  }

  MultiValBin* CreateLike(int num_bin, int,
                          double estimate_element_per_row) const override {
    return new MultiValSparseBin<VAL_T>(num_data_, num_bin,
                                        estimate_element_per_row);
  }

  void ReSizeForSubFeature(int num_bin, int,
                           double estimate_element_per_row) override {
    num_bin_ = num_bin;
    estimate_element_per_row_ = estimate_element_per_row;
    data_size_t estimate_num_data =
        static_cast<data_size_t>(num_data_ * estimate_element_per_row_ * 1.1);
    size_t npart = 1 + t_data_.size();
    data_size_t avg_num_data =
        static_cast<data_size_t>(estimate_num_data / npart);
    if (static_cast<data_size_t>(data_.size()) < avg_num_data) {
      data_.resize(avg_num_data, 0);
    }
    for (size_t i = 0; i < t_data_.size(); ++i) {
      if (static_cast<data_size_t>(t_data_[i].size()) < avg_num_data) {
        t_data_[i].resize(avg_num_data, 0);
      }
    }
  }

  void CopySubFeature(const MultiValBin* full_bin, const std::vector<int>&,
                      const std::vector<uint32_t>& lower,
                      const std::vector<uint32_t>& upper,
                      const std::vector<uint32_t>& delta) override {
    const auto other =
        reinterpret_cast<const MultiValSparseBin<VAL_T>*>(full_bin);
    int num_threads = 1;
#pragma omp parallel
#pragma omp master
    { num_threads = omp_get_num_threads(); }

    const int min_block_size = 1024;
    const int n_block = std::min(
        num_threads, (num_data_ + min_block_size - 1) / min_block_size);
    const data_size_t block_size = (num_data_ + n_block - 1) / n_block;
    std::vector<data_size_t> sizes(t_data_.size() + 1, 0);
    const int pre_alloc_size = 50;
#pragma omp parallel for schedule(static, 1)
    for (int tid = 0; tid < n_block; ++tid) {
      data_size_t start = tid * block_size;
      data_size_t end = std::min(num_data_, start + block_size);
      auto& buf = (tid == 0) ? data_ : t_data_[tid - 1];
      data_size_t size = 0;
      for (data_size_t i = start; i < end; ++i) {
        const auto j_start = other->RowPtr(i);
        const auto j_end = other->RowPtr(i + 1);
        if (size + (j_end - j_start) > static_cast<data_size_t>(buf.size())) {
          buf.resize(size + (j_end - j_start) * pre_alloc_size);
        }
        int k = 0;
        const data_size_t pre_size = size;
        for (auto j = j_start; j < j_end; ++j) {
          auto val = other->data_[j];
          while (val >= upper[k]) {
            ++k;
          }
          if (val >= lower[k]) {
            buf[size++] = static_cast<VAL_T>(val - delta[k]);
          }
        }
        row_ptr_[i + 1] = size - pre_size;
      }
      sizes[tid] = size;
    }
    MergeData(sizes.data());
  }

  inline data_size_t RowPtr(data_size_t idx) const { return row_ptr_[idx]; }

  MultiValSparseBin<VAL_T>* Clone() override;

 private:
  data_size_t num_data_;
  int num_bin_;
  double estimate_element_per_row_;
  std::vector<VAL_T, Common::AlignmentAllocator<VAL_T, 32>> data_;
  std::vector<data_size_t, Common::AlignmentAllocator<data_size_t, 32>>
      row_ptr_;
  std::vector<std::vector<VAL_T, Common::AlignmentAllocator<VAL_T, 32>>>
      t_data_;
  std::vector<data_size_t> t_size_;

  MultiValSparseBin<VAL_T>(const MultiValSparseBin<VAL_T>& other)
      : num_data_(other.num_data_),
        num_bin_(other.num_bin_),
        estimate_element_per_row_(other.estimate_element_per_row_),
        data_(other.data_),
        row_ptr_(other.row_ptr_) {}
};

template <typename VAL_T>
MultiValSparseBin<VAL_T>* MultiValSparseBin<VAL_T>::Clone() {
  return new MultiValSparseBin<VAL_T>(*this);
}

}  // namespace LightGBM
#endif  // LIGHTGBM_IO_MULTI_VAL_SPARSE_BIN_HPP_
