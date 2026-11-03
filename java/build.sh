#!/usr/bin/env sh
set -eu
# JDK 17+；固定依赖版本及摘要，与 Windows 构建使用相同源码。
cd "$(dirname "$0")"
mkdir -p lib out
if [ ! -f lib/gson-2.11.0.jar ]; then
 curl -fL https://repo.maven.apache.org/maven2/com/google/code/gson/gson/2.11.0/gson-2.11.0.jar -o lib/gson-2.11.0.jar
fi
EXPECTED=57928d6e5a6edeb2abd3770a8f95ba44dce45f3b23b7a9dc2b309c581552a78b
ACTUAL=$(openssl dgst -sha256 lib/gson-2.11.0.jar | sed 's/^.*= //')
[ "$EXPECTED" = "$ACTUAL" ] || { echo 'Gson checksum mismatch' >&2; exit 1; }
javac --release 17 -encoding UTF-8 -cp lib/gson-2.11.0.jar -d out src/main/java/io/embedai/*.java
jar --create --file EmbedAiChatJava.jar -C out .
