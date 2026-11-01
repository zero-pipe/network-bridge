#ifndef NB_CODEC_JSON_INTERNAL_H
#define NB_CODEC_JSON_INTERNAL_H

/*
 * codec_json 内部入口：对已经解析好的 JSON 对象（cJSON*）直接填充模型，
 * 避免配置层为了复用模型而重新序列化子对象。
 *
 * 该头只在库内部使用，不对外暴露 cJSON 依赖。
 */

#include "codec_json.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cJSON;

/*
 * 用 model 填充 obj（obj 必须已清零）。node 必须是 cJSON 对象。
 * 返回值同 nb_codec_json_*。
 */
int nb_codec_json_fill_object(const struct cJSON *node,
                              const nb_codec_json_model_t *model,
                              size_t model_count,
                              void *obj);

/*
 * Same as nb_codec_json_fill_object, but on failure stores the model key of
 * the field that failed in *failed_key (NULL when the failure is not tied to a
 * field). This lets config report "runtime.io_threads" instead of just
 * "runtime".
 */
int nb_codec_json_fill_object_ex(const struct cJSON *node,
                                 const nb_codec_json_model_t *model,
                                 size_t model_count,
                                 void *obj,
                                 const char **failed_key);

#ifdef __cplusplus
}
#endif

#endif /* NB_CODEC_JSON_INTERNAL_H */
