//! 独立控制台入口，业务功能均由可复用核心库实现。
/// 传递参数并把启动失败转换为非零退出码。
fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Err(error) = embed_ai_chat::console::run(&args) {
        eprintln!("Error: {error}");
        std::process::exit(1);
    }
}
