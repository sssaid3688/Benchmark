file(REMOVE_RECURSE
  "libcutlass_conv3d_sm100_fprop_f16ndhwc_f16ndhwc_f16_f16_f16ndhwc.pdb"
  "libcutlass_conv3d_sm100_fprop_f16ndhwc_f16ndhwc_f16_f16_f16ndhwc.so"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_conv3d_sm100_fprop_f16ndhwc_f16ndhwc_f16_f16_f16ndhwc.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
