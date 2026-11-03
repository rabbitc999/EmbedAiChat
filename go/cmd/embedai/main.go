package main

import (
	"embedai.chat/framework/embedai"
	"fmt"
	"os"
)

// main 运行独立控制台；启动失败采用非零退出码，交互错误由控制台自行处理。
func main() {
	if err := embedai.RunConsole(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "Error:", err)
		os.Exit(1)
	}
}
