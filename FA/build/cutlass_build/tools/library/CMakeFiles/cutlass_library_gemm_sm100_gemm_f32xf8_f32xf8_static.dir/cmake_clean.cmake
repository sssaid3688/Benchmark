file(REMOVE_RECURSE
  "libcutlass_gemm_sm100_gemm_f32xf8_f32xf8.a"
  "libcutlass_gemm_sm100_gemm_f32xf8_f32xf8.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_gemm_sm100_gemm_f32xf8_f32xf8_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
