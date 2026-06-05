file(REMOVE_RECURSE
  "libcutlass_conv2d_sm100_fprop_e4m3nhwc_e4m3nhwc_f32_bf16_bf16nhwc.pdb"
  "libcutlass_conv2d_sm100_fprop_e4m3nhwc_e4m3nhwc_f32_bf16_bf16nhwc.so"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_conv2d_sm100_fprop_e4m3nhwc_e4m3nhwc_f32_bf16_bf16nhwc.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
