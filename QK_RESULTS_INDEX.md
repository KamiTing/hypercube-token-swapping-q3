# Qk Special Case Results Index

This file records the result directories that should be kept as official or
useful comparison artifacts.

## Primary Artifacts

- `output/qk_special_cases_routes.xlsx`
  - Complete route workbook.
  - Contains Q4 through Q12 sheets.
  - Q12 currently contains `q12_case1` only.

## Official / Current Result Sources

- `output/q4_q11_cuda_all_bw512_pool65536_win65536_restart2048_plateau2048_perturb010_cub_20260613_113055/`
  - CUDA run covering Q4 through Q11.
  - Useful as the compact current comparison source for Q4-Q11.

- `output/q12_cuda_gpu_retained_bw1024_pool262144_win65536_restart4096_plateau1536_perturb015_cub_path_20260613_132118/`
  - Official Q12 `case1` result.
  - `beam_status=solved`, `beam_steps=28814`, `beam_path_valid=1`.

## Supplemental Best-Path Sources

- `output/q11_cuda_cub_bw512_pool65536_win65536_restart2048_plateau2048_perturb010_path_20260612_231014/`
  - Earlier Q11 CUDA CUB result.
  - Kept because some Q11 paths are better than the later all-run source.

- `output/q9_cuda_diverse_bw256_pool4096_win4096_perturb010_20260613_090936/`
  - Q9 CUDA diverse comparison result.

- `output/q8_case1_trim_20260605_154219/`
- `output/q8_case2_trim_20260605_154927/`
- `output/q9_case1_disk_bw256_20260607_113040/`
- `output/q9_case2_disk_bw256_path_20260609_170556/`
- `output/cuda_opt_verify_q10_bw512_case1/`
- `output/cuda_fix_verify_q10_case2/`
  - Historical best-path or verification sources used by workbook update scripts.

## Other Preserved Benchmarks

- `output/q4_path_selected_10000_basic_no_path_20260604_205233/`
  - Tracked Q4 random benchmark output referenced by `README.md`.
  - Not part of the Qk special-case route workbook, but kept because it is a
    documented benchmark artifact.

## Cleanup Policy

Delete after small tests unless explicitly promoted to an official result:

- empty run directories
- aborted runs with empty `qk_special_cases.csv`
- `verify_*` smoke directories
- preview renders that can be regenerated
- stale `qk_beam_path_*.bin` temporary files
