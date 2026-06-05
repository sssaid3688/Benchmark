file(REMOVE_RECURSE
  "libcutlass_conv3d_sm100_wgrad_bf16ndhwc_bf16ndhwc_f32_bf16_bf16kcsrt.a"
  "libcutlass_conv3d_sm100_wgrad_bf16ndhwc_bf16ndhwc_f32_bf16_bf16kcsrt.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_conv3d_sm100_wgrad_bf16ndhwc_bf16ndhwc_f32_bf16_bf16kcsrt_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
