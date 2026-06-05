file(REMOVE_RECURSE
  "libcutlass_gemm_sm100_void_gemm_e3m2_e2m3.a"
  "libcutlass_gemm_sm100_void_gemm_e3m2_e2m3.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_gemm_sm100_void_gemm_e3m2_e2m3_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
