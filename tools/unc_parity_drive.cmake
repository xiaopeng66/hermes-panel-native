# unc_parity_drive.cmake — ctest 第二步：跑 tools/unc_parity_drive.py（apply.py 逐字段对拍）。
# exe（unc-parity-snapshot）必须先跑完（DEPENDS 已声明），夹具与 C++ 快照都在 tmp/unc-parity/。
find_program(PYTHON_EXE NAMES python python3 py REQUIRED)
execute_process(
    COMMAND "${PYTHON_EXE}" "${CMAKE_CURRENT_LIST_DIR}/unc_parity_drive.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT 200
)
message("${out}${err}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "unc-parity driver failed (rc=${rc})")
endif()
