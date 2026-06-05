file(REMOVE_RECURSE
  "libcutlass_conv3d_sm100_fprop_e4m3ndhwc_e4m3ndhwc_f32_f16_f16ndhwc.a"
  "libcutlass_conv3d_sm100_fprop_e4m3ndhwc_e4m3ndhwc_f32_f16_f16ndhwc.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_conv3d_sm100_fprop_e4m3ndhwc_e4m3ndhwc_f32_f16_f16ndhwc_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
