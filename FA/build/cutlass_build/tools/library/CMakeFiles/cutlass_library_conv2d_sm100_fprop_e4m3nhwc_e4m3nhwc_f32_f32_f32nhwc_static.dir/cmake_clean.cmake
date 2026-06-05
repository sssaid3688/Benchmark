file(REMOVE_RECURSE
  "libcutlass_conv2d_sm100_fprop_e4m3nhwc_e4m3nhwc_f32_f32_f32nhwc.a"
  "libcutlass_conv2d_sm100_fprop_e4m3nhwc_e4m3nhwc_f32_f32_f32nhwc.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_conv2d_sm100_fprop_e4m3nhwc_e4m3nhwc_f32_f32_f32nhwc_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
