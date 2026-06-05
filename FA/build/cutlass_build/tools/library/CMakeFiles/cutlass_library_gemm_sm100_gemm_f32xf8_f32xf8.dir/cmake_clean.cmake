file(REMOVE_RECURSE
  "libcutlass_gemm_sm100_gemm_f32xf8_f32xf8.pdb"
  "libcutlass_gemm_sm100_gemm_f32xf8_f32xf8.so"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_gemm_sm100_gemm_f32xf8_f32xf8.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
