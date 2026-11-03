//go:build windows

package embedai

import (
	"os/exec"
	"strconv"
	"syscall"
)

// configureProcess 隐藏本客户端启动的 MCP 窗口，参数依然由 exec 直接传递。
func configureProcess(command *exec.Cmd) {
	command.SysProcAttr = &syscall.SysProcAttr{CreationFlags: 0x08000000}
}

// killProcess 清理自身启动的进程树，失败时至少终止直接子进程。
func killProcess(command *exec.Cmd) {
	if command.Process != nil {
		kill := exec.Command("taskkill", "/PID", strconv.Itoa(command.Process.Pid), "/T", "/F")
		configureProcess(kill)
		_ = kill.Run()
		_ = command.Process.Kill()
	}
}
