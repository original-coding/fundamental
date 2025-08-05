# 安装
``` powershell
git clone https://github.com/microsoft/vcpkg.git
$env:ALL_PROXY = "http://127.0.0.1:1080"  # 全局代理
$env:HTTPS_PROXY = "http://127.0.0.1:1080" # HTTPS 流量
$env:HTTP_PROXY = "http://127.0.0.1:1080" # HTTP 流量
cd vcpkg
./bootstrap-vcpkg.bat
./vcpkg integrate install
#将vcpkg目录添加到PATH
```
# 创建mainfest文件
vcpkg.json
``` json
{
    "name": "fundamental",
    "version": "1.0",
    "dependencies": [
        "openssl"
    ]
}
```

``` cmd
:: 进入vcpkg.json所在目录
:: 设置代理
set http_proxy=http://127.0.0.1:1080
set https_proxy=http://127.0.0.1:1080
:: 指定安装目录，不要安装在本地
vcpkg install --x-install-root=%VCPKG_ROOT%\installed
```