//go:build !windows

package embedai

import (
	"os/exec"
	"syscall"
)

// configureProcess 创建独立进程组，使清理范围只包含本客户端的子进程树。
func configureProcess(command *exec.Cmd) { command.SysProcAttr = &syscall.SysProcAttr{Setpgid: true} }

// killProcess 关闭所属进程组和直接子进程，不重试任何业务操作。
func killProcess(command *exec.Cmd) {
	if command.Process != nil {
		_ = syscall.Kill(-command.Process.Pid, syscall.SIGKILL)
		_ = command.Process.Kill()
	}
}
