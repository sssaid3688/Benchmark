file(REMOVE_RECURSE
  "libcutlass_conv2d_sm100_wgrad_bf16nhwc_bf16nhwc_f32_bf16_bf16kcsr.a"
  "libcutlass_conv2d_sm100_wgrad_bf16nhwc_bf16nhwc_f32_bf16_bf16kcsr.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_conv2d_sm100_wgrad_bf16nhwc_bf16nhwc_f32_bf16_bf16kcsr_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
