// ort_loader.hpp - Dynamic ONNX Runtime C API loader for camswap daemon

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace camswap {
namespace ai {
namespace ort {

// Minimal subset of ONNX Runtime C API v11+ required for session creation,
// tensor inspection, and preallocated tensor execution.
struct OrtEnv;
struct OrtStatus;
struct OrtMemoryInfo;
struct OrtSession;
struct OrtSessionOptions;
struct OrtValue;
struct OrtRunOptions;
struct OrtTypeInfo;
struct OrtTensorTypeAndShapeInfo;
struct OrtAllocator;

enum OrtLoggingLevel {
  ORT_LOGGING_LEVEL_VERBOSE = 0,
  ORT_LOGGING_LEVEL_INFO = 1,
  ORT_LOGGING_LEVEL_WARNING = 2,
  ORT_LOGGING_LEVEL_ERROR = 3,
  ORT_LOGGING_LEVEL_FATAL = 4,
};

enum GraphOptimizationLevel {
  ORT_DISABLE_ALL = 0,
  ORT_ENABLE_BASIC = 1,
  ORT_ENABLE_EXTENDED = 2,
  ORT_ENABLE_ALL = 99
};

enum ONNXTensorElementDataType {
  ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED = 0,
  ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT = 1,
  ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8 = 2,
  ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8 = 3,
};

enum OrtAllocatorType {
  OrtInvalidAllocator = -1,
  OrtDeviceAllocator = 0,
  OrtArenaAllocator = 1
};

enum OrtMemType {
  OrtMemTypeCPUInput = -2,
  OrtMemTypeCPUOutput = -1,
  OrtMemTypeCPU = OrtMemTypeCPUOutput,
  OrtMemTypeDefault = 0
};

// Table of resolved C API pointers used by FaceLandmarker and FaceSwap.
struct OrtApiTable {
  const void *raw_api;
  const char *(*GetErrorMessage)(const OrtStatus *status);
  void (*ReleaseStatus)(OrtStatus *input);

  OrtStatus *(*CreateEnv)(OrtLoggingLevel log_severity_level, const char *logid,
                          OrtEnv **out);
  void (*ReleaseEnv)(OrtEnv *input);

  OrtStatus *(*CreateSessionOptions)(OrtSessionOptions **options);
  OrtStatus *(*SetIntraOpNumThreads)(OrtSessionOptions *options,
                                     int intra_op_num_threads);
  OrtStatus *(*SetSessionGraphOptimizationLevel)(
      OrtSessionOptions *options,
      GraphOptimizationLevel graph_optimization_level);
  void (*ReleaseSessionOptions)(OrtSessionOptions *input);

  OrtStatus *(*CreateSession)(const OrtEnv *env, const char *model_path,
                              const OrtSessionOptions *options,
                              OrtSession **out);
  void (*ReleaseSession)(OrtSession *input);

  OrtStatus *(*SessionGetInputCount)(const OrtSession *session, size_t *out);
  OrtStatus *(*SessionGetOutputCount)(const OrtSession *session, size_t *out);
  OrtStatus *(*SessionGetInputTypeInfo)(const OrtSession *session, size_t index,
                                        OrtTypeInfo **type_info);
  OrtStatus *(*SessionGetOutputTypeInfo)(const OrtSession *session,
                                         size_t index, OrtTypeInfo **type_info);
  OrtStatus *(*SessionGetInputName)(const OrtSession *session, size_t index,
                                    OrtAllocator *allocator, char **value);
  OrtStatus *(*SessionGetOutputName)(const OrtSession *session, size_t index,
                                     OrtAllocator *allocator, char **value);

  OrtStatus *(*GetAllocatorWithDefaultOptions)(OrtAllocator **out);
  OrtStatus *(*AllocatorFree)(OrtAllocator *ptr, void *p);

  OrtStatus *(*CastTypeInfoToTensorInfo)(
      const OrtTypeInfo *type_info, const OrtTensorTypeAndShapeInfo **out);
  void (*ReleaseTypeInfo)(OrtTypeInfo *input);
  OrtStatus *(*GetDimensionsCount)(const OrtTensorTypeAndShapeInfo *info,
                                   size_t *out);
  OrtStatus *(*GetDimensions)(const OrtTensorTypeAndShapeInfo *info,
                              int64_t *dim_values, size_t dim_values_length);

  OrtStatus *(*CreateCpuMemoryInfo)(enum OrtAllocatorType type,
                                    enum OrtMemType mem_type,
                                    OrtMemoryInfo **out);
  void (*ReleaseMemoryInfo)(OrtMemoryInfo *input);

  OrtStatus *(*CreateTensorWithDataAsOrtValue)(
      const OrtMemoryInfo *info, void *p_data, size_t p_data_len,
      const int64_t *shape, size_t shape_len, ONNXTensorElementDataType type,
      OrtValue **out);
  OrtStatus *(*GetTensorMutableData)(OrtValue *value, void **out);
  OrtStatus *(*GetTensorTypeAndShape)(const OrtValue *value,
                                      OrtTensorTypeAndShapeInfo **out);
  void (*ReleaseTensorTypeAndShapeInfo)(OrtTensorTypeAndShapeInfo *input);
  void (*ReleaseValue)(OrtValue *input);

  OrtStatus *(*Run)(OrtSession *session, const OrtRunOptions *run_options,
                    const char *const *input_names,
                    const OrtValue *const *inputs, size_t input_len,
                    const char *const *output_names, size_t output_names_len,
                    OrtValue **outputs);
};

// Loads libonnxruntime.so dynamically if present and resolves the C API table.
bool ensure_ort_loaded(OrtApiTable *out_table);

} // namespace ort
} // namespace ai
} // namespace camswap
