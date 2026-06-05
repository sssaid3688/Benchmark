file(REMOVE_RECURSE
  "libcutlass_conv3d_sm100_wgrad_f16ndhwc_f16ndhwc_f32_f16_f16kcsrt.a"
  "libcutlass_conv3d_sm100_wgrad_f16ndhwc_f16ndhwc_f32_f16_f16kcsrt.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_conv3d_sm100_wgrad_f16ndhwc_f16ndhwc_f32_f16_f16kcsrt_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
