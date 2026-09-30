.intel_syntax noprefix

.extern task_exit

.global task_return_stub
.type task_return_stub,@function
task_return_stub:
    call task_exit
    ud2
