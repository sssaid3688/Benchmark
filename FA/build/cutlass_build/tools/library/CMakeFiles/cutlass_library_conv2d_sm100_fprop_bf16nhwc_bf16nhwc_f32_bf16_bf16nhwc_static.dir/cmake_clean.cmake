file(REMOVE_RECURSE
  "libcutlass_conv2d_sm100_fprop_bf16nhwc_bf16nhwc_f32_bf16_bf16nhwc.a"
  "libcutlass_conv2d_sm100_fprop_bf16nhwc_bf16nhwc_f32_bf16_bf16nhwc.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_conv2d_sm100_fprop_bf16nhwc_bf16nhwc_f32_bf16_bf16nhwc_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
