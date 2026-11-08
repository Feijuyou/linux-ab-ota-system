# 虚拟机交叉编译：从板子复制 sysroot

当前方案：保留虚拟机原来的软件源和交叉编译器，通过 SCP 复制板子的头文件和库，在虚拟机编译，再将程序传到 RDK X3 运行。不使用官方 SDK，也不需要在虚拟机安装 ARM64 curl 软件包。

sysroot 是目标系统编译文件的副本，不是启动程序或 boot 分区。这里用于为编译器提供板子的系统头文件、ARM64 库和动态加载器。

## 1. 撤销之前的虚拟机软件源改动

以下命令仅针对本次添加的架构标记及 `rdk-arm64.list`，原来的镜像地址保持不变。用户已说明 ARM64 软件包没有安装成功。

在虚拟机逐条执行：

```bash
# 如果提示架构仍被软件包使用，先停下，查看报错，不要强制卸载。
sudo dpkg --remove-architecture arm64

# 撤销之前给原软件源添加的 [arch=amd64]。
sudo sed -i 's/^deb \[arch=amd64\] http/deb http/' /etc/apt/sources.list

# 删除本次专门添加的 ARM64 软件源文件。
sudo rm -f /etc/apt/sources.list.d/rdk-arm64.list
sudo apt update
```

先前创建的 `/etc/apt/sources.list.backup.*` 备份保留。上述命令用于尚未成功安装 ARM64 软件包的情况；如果 dpkg 拒绝移除架构，先核对实际的软件包状态。

## 2. 打包板子的开发文件并通过 SCP 复制

以下命令都在虚拟机执行。SSH 在板子上运行 tar，SCP 再将压缩包传回虚拟机。

```bash
# -h 展开符号链接，将它们指向的实际文件打包。
# 这样解压后不会把板子上的绝对链接误解析到虚拟机的系统库。
ssh sunrise@192.168.127.10 \
  'tar -czhf "$HOME/rdk-sysroot.tar.gz" -C / usr/include usr/lib/aarch64-linux-gnu lib/aarch64-linux-gnu lib/ld-linux-aarch64.so.1'

# 复制压缩包。冒号后是板子用户家目录中的文件。
scp sunrise@192.168.127.10:rdk-sysroot.tar.gz "$HOME/rdk-sysroot.tar.gz"

# 解压到虚拟机本地 Linux 目录，保留 Linux 文件属性。
mkdir -p "$HOME/rdk-sysroot"
tar -xzf "$HOME/rdk-sysroot.tar.gz" -C "$HOME/rdk-sysroot"
```

逐条执行，某一步失败先处理报错。文件量可能较大；打包过程在板子家目录中新建压缩包，不修改原来的系统文件。

复制内容包括系统头文件、ARM64 多架构库目录和动态加载器，足够覆盖当前 curl 程序的常规 Ubuntu 依赖；以后使用 `/usr/local` 下的自定义库或厂商专用库时，需要补充对应文件。

检查复制结果：

```bash
ls "$HOME/rdk-sysroot/usr/include/stdio.h"
find "$HOME/rdk-sysroot/usr/include" -path '*/curl/curl.h'
file -L "$HOME/rdk-sysroot/usr/lib/aarch64-linux-gnu/libcurl.so"
```

应找到头文件，库文件应显示 `ARM aarch64`。如果板子缺少头文件或开发链接文件，需要先补齐板端开发文件，再重新复制。仅有版本化运行库 `libcurl.so.4` 不等于具备用于链接的开发文件 `libcurl.so`。

## 3. 在虚拟机编译

```bash
cd ~/OTA_Server
./build.sh
```

默认 sysroot 是 `$HOME/rdk-sysroot`，也可用 `RDK_SYSROOT=/实际路径 ./build.sh` 覆盖。若此前设置过 `RDK_SYSROOT`，请核对脚本打印的路径。

`build.sh` 查找交叉编译器的完整路径；工具链指定 Linux / ARM64 和 sysroot；`CMakeLists.txt` 只从该 sysroot 查找 curl。脚本会重新查找 curl，避免沿用旧的多架构库路径。

如果切换 sysroot 后 CMake 提示编译器或 ABI 缓存问题，可保留旧构建目录后重建：

```bash
mv build-rdk "build-rdk.backup.$(date +%Y%m%d-%H%M%S)"
./build.sh
```

成功后生成 `build-rdk/ota_client`；文件信息应包含 `ELF 64-bit` 和 `ARM aarch64`。这证明架构正确，运行兼容性还需要传板验证。交叉编译器自身提供的 C++ 标准库开发文件也会参与构建，所以使用板端 sysroot 不代表任意编译器版本都兼容。

## 4. 传到板子运行

在虚拟机执行：

```bash
scp build-rdk/ota_client sunrise@192.168.127.10:~/ota_client
ssh sunrise@192.168.127.10
```

登录板子后执行：

```bash
chmod +x ~/ota_client
ldd ~/ota_client
~/ota_client
```

`ldd` 不应出现 `not found`；如提示 `GLIBC_...` 或 `GLIBCXX_...` 版本缺失，需要核对编译侧与板端库版本。

当前程序访问 `http://192.168.248.128:8000/manifest.json`。在虚拟机另一个终端提供服务：

```bash
cd ~/OTA_Server
python3 -m http.server 8000 --bind 0.0.0.0
```

板子需要能访问该地址。程序当前只获取并打印 manifest，还没有下载、校验或安装固件的功能。
