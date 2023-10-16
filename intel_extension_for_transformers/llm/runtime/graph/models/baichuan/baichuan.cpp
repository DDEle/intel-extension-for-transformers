//  Copyright (c) 2023 Intel Corporation
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/data_types.h"
#include "core/ne.h"
#include "core/ne_layers.h"
#include "core/ne_jblas.h"
#include "core/layers/mha_dense.h"
#include "models/model_utils/model_config.h"
#include "models/model_utils/model_utils.h"
#include "models/model_utils/util.h"

// evaluate the transformer
//
//   - lctx:      model context
//   - tokens:    new batch of tokens to process
//   - n_past:    the offset to which the kv is cached to
//   - n_total:   the number of tokens evaluated so far (including evicted tokens if there is any)
//   - n_threads: number of threads to use
//
static int flag = 0;
static int first_tokens_size = 0;
static bool baichuan_model_eval_internal(model_context& lctx, const model_token* tokens, const int n_tokens,
                                         const int n_past, const int n_total, const int n_threads) {
  const int64_t t_start_us = ne_time_us();

  const int N = n_tokens;

  const int batch_size = lctx.batch_size;
  const auto& model = lctx.model;
  const auto& hparams = model.hparams;

  const auto& kv_self = model.kv_self;

  MODEL_ASSERT(!!kv_self.ctx);

  const int n_embd = hparams.n_embd;
  const int n_layer = hparams.n_layer;
  const int n_ctx = lctx.n_ctx;  // max number fo tokens to keep in the kv-cache
  const int n_keep = lctx.n_keep;

  // The streaming-llm way of kv-caching. Ref: https://arxiv.org/abs/2309.17453
  NE_ASSERT(("First token should not be greater then n_ctx!", N < n_ctx));
  NE_ASSERT(("Unmatched n_past / n_total pair.", n_past == n_keep + (n_total - n_keep) % (n_ctx - n_keep)));
  // total number of tokens cached after this round of model_eval
  const int n_cached = std::min(n_ctx, n_total + N);

  if (flag == 0) {
    first_tokens_size = n_tokens;
    flag++;
  }

  const int n_head = hparams.n_head;
  const int n_vocab = hparams.n_vocab;
  const int n_rot = n_embd / n_head / 2;
  const int num_attention_heads = n_head;

  auto& mem_per_token = lctx.mem_per_token;
  auto& buf_compute = lctx.buf_compute;

  struct ne_init_params params = {
      /*.mem_size   =*/buf_compute.size,
      /*.mem_buffer =*/buf_compute.addr,
      /*.no_alloc   =*/false,
  };

  struct ne_context* ctx0 = ne_init(params);

  // for big probaichuans, if BLAS is enabled, it is better to use only one thread
  // otherwise, the threads are spin-lock waiting for the BLAS calls and are degrading the performance
  ne_cgraph gf = {};
  gf.n_threads = N >= 32 && ne_cpu_has_blas() ? 1 : n_threads;

  struct ne_tensor* embd = d_ne_new_tensor_1d(ctx0, NE_TYPE_I32, N);
  memcpy(embd->data, tokens, N * ne_element_size(embd));

  struct ne_tensor* inpL = ne_get_rows(ctx0, model.others[0], embd);
  int hidden_size = inpL->ne[0];
  int qlen = inpL->ne[1];
  int head_size = hidden_size / num_attention_heads;
  for (int il = 0; il < n_layer; ++il) {
    struct ne_tensor* cur;

    lctx.use_buf(ctx0, 0);

    struct ne_tensor* residual = inpL;

    // LayerNorm
    cur = ne_rms_norm(ctx0, inpL);
    cur = ne_mul(ctx0, cur, model.layers[il].norm[0]);
    // SelfAttention
    {
      // Linear::forward compute QKV
      cur = ne_mul_mat(ctx0, model.layers[il].attn[0], cur);

      ne_tensor* query_layer = ne_view_3d(ctx0, cur, head_size, n_head, N, head_size * ne_element_size(cur), cur->nb[1],
                                          0);                   // [qlen, hidden]
      query_layer = ne_permute(ctx0, query_layer, 0, 2, 1, 3);  // [heads, qlen, head_size]

      ne_tensor* key_layer =
          ne_view_3d(ctx0, cur, head_size, num_attention_heads, qlen, head_size * ne_element_size(cur), cur->nb[1],
                     hidden_size * ne_element_size(cur));
      key_layer = ne_permute(ctx0, key_layer, 0, 2, 1, 3);  // [heads, qlen, head_size]

      ne_tensor* value_layer =
          ne_view_3d(ctx0, cur, head_size, num_attention_heads, qlen, head_size * ne_element_size(cur), cur->nb[1],
                     2 * hidden_size * ne_element_size(cur));   // [qlen, heads, head_size]
      value_layer = ne_permute(ctx0, value_layer, 1, 2, 0, 3);  // [heads, head_size, qlen]

      // store key and value to memory
      {
        struct ne_tensor* k_cache_view =
            ne_view_3d(ctx0, model.layers[il].k_cache, head_size, qlen, num_attention_heads,
                       model.layers[il].k_cache->nb[1], model.layers[il].k_cache->nb[2],
                       n_past * head_size * ne_element_size(model.layers[il].k_cache));  // [kv_heads, qlen, head_size]

        struct ne_tensor* v_cache_view =
            ne_view_3d(ctx0, model.layers[il].v_cache, qlen, head_size, num_attention_heads,
                       model.layers[il].v_cache->nb[1], model.layers[il].v_cache->nb[2],
                       n_past * ne_element_size(model.layers[il].v_cache));  // [kv_heads, head_size, qlen]

        ne_build_forward_expand(&gf, ne_cpy(ctx0, key_layer, k_cache_view));
        ne_build_forward_expand(&gf, ne_cpy(ctx0, value_layer, v_cache_view));
      }
      // concat key & value with past kv
      key_layer = ne_view_3d(ctx0, model.layers[il].k_cache, head_size, n_cached, num_attention_heads,
                             model.layers[il].k_cache->nb[1], model.layers[il].k_cache->nb[2],
                             0);  // [kv_heads, klen, head_size]
      value_layer = ne_view_3d(ctx0, model.layers[il].v_cache, n_cached, head_size, num_attention_heads,
                               model.layers[il].v_cache->nb[1], model.layers[il].v_cache->nb[2],
                               0);  // [kv_heads, head_size, klen]

      // attention
      struct ne_tensor* attn_scores = ne_mul_mat(ctx0, key_layer, query_layer);  // [heads, qlen, klen]
      attn_scores = ne_scale_inplace(ctx0, attn_scores, ne_new_f32(ctx0, 1.f / std::sqrt(head_size)));
      attn_scores = ne_alibi(ctx0, attn_scores, n_total, num_attention_heads, 8);
      if (n_total == 0) {
        attn_scores = ne_diag_mask_inf_inplace(ctx0, attn_scores, n_total);
      }
      ne_tensor* attn_probs = ne_soft_max_inplace(ctx0, attn_scores);  // [heads, qlen, klen]

      // ne_compute_forward_mul_mat_f16_f32
      ne_tensor* context_layer = ne_mul_mat(ctx0, value_layer, attn_probs);  // [heads, qlen, head_size]
      context_layer = ne_cont(ctx0, ne_permute(ctx0, context_layer, 0, 2, 1, 3));
      context_layer = ne_reshape_2d(ctx0, context_layer, hidden_size, qlen);

      // F32 mul_mat
      cur = ne_mul_mat(ctx0, model.layers[il].attn[1], context_layer);
    }

    lctx.use_buf(ctx0, 1);
    cur = ne_add_inplace(ctx0, cur, residual);
    residual = cur;

    // post_attention_layernorm
    struct ne_tensor* hidden_states = ne_rms_norm(ctx0, cur);
    hidden_states = ne_mul(ctx0, hidden_states, model.layers[il].norm[1]);

    // mlp.forward
    struct ne_tensor* gate = ne_mul_mat(ctx0, model.layers[il].ffn[0], hidden_states);
    gate = ne_silu(ctx0, gate);
    struct ne_tensor* up = ne_mul_mat(ctx0, model.layers[il].ffn[1], hidden_states);
    struct ne_tensor* mlp_output = ne_mul(ctx0, gate, up);
    mlp_output = ne_mul_mat(ctx0, model.layers[il].ffn[2], mlp_output);

    inpL = ne_add_inplace(ctx0, mlp_output, residual);
  }

  lctx.use_buf(ctx0, 0);
  // used at the end to optionally extract the embeddings
  struct ne_tensor* embeddings = NULL;
  // norm
  {
    inpL = ne_rms_norm(ctx0, inpL);
    inpL = ne_mul(ctx0, inpL, model.others[1]);
  }

  lctx.use_buf(ctx0, -1);
  if (embd->ne[0] > 1) {
    inpL = ne_view_1d(ctx0, inpL, hidden_size, (embd->ne[0] - 1) * hidden_size * ne_element_size(inpL));
  }

  // lm_head
  inpL = ne_mul_mat(ctx0, model.others[2], inpL);

  ne_build_forward_expand(&gf, inpL);
  ne_graph_compute(ctx0, &gf);

#ifdef NE_PERF
  bool engine_profiling_ = (getenv("ENGINE_PROFILING") != NULL);
  if (engine_profiling_) {
    ne_graph_profiling(&gf);
  }
#endif

  // update kv token count
  lctx.model.kv_self.n = n_cached;

  // extract logits
  {
    auto& logits_out = lctx.logits;

    if (lctx.logits_all) {
      logits_out.resize(n_vocab * N);
      memcpy(logits_out.data(), (float*)ne_get_data(inpL), sizeof(float) * n_vocab * N);
    } else {
      // return result for just the last token
      logits_out.resize(n_vocab);
      memcpy(logits_out.data(), (float*)ne_get_data(inpL), sizeof(float) * n_vocab);
    }
  }

  // extract embeddings
  if (!lctx.embedding.empty()) {
    auto& embedding_out = lctx.embedding;

    embedding_out.resize(n_embd);
    memcpy(embedding_out.data(), (float*)ne_get_data(embeddings) + (n_embd * (N - 1)), sizeof(float) * n_embd);
  }

  if (mem_per_token == 0) {
    mem_per_token = ne_used_mem(ctx0) / N;
  }

  ne_free(ctx0);

  // measure the performance only for the single-token evals
  int64_t time_interval = ne_time_us() - t_start_us;
  if (N == 1) {
    lctx.t_eval_us += time_interval;
    lctx.n_eval++;
  } else if (N > 1) {
    lctx.t_p_eval_us += time_interval;
    lctx.n_p_eval += N;
  }
  lctx.eval_times.push_back(time_interval);

  return true;
}

int model_eval(struct model_context* ctx, const model_token* tokens, int n_tokens, int n_past, int n_total,
               int n_threads) {
  if (!baichuan_model_eval_internal(*ctx, tokens, n_tokens, n_past, n_total, n_threads)) {
    fprintf(stderr, "%s: failed to eval\n", __func__);
    return 1;
  }

  // get a more accurate load time, upon first eval
  // TODO: fix this
  if (!ctx->has_evaluated_once) {
    ctx->t_load_us = ne_time_us() - ctx->t_start_us;
    ctx->has_evaluated_once = true;
  }

  return 0;
}
