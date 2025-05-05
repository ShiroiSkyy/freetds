@echo off
echo "准备ICONV工作流"
curl https://codeload.github.com/win-iconv/win-iconv/zip/v0.0.8 -o win_iconv.zip
7z x win_iconv.zip
mv win-iconv-0.0.8 iconv
wsl
mkdir iconv-build
exit
cd iconv-build

#cmake -G "NMake Makefiles" -DBUILD_STATIC=on -DBUILD_SHARED=off -DBUILD_EXECUTABLE=off -DBUILD_TEST=off -DCMAKE_BUILD_TYPE=Release ../iconv、
echo "CMake 配置 ICONV"
cmake -G "NMake Makefiles" -DBUILD_STATIC=on -DBUILD_SHARED=off -DBUILD_EXECUTABLE=off -DBUILD_TEST=off -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ../iconv
nmake

cd ..

echo "准备FreeTDS工作流"
wsl
mkdir -p ./build/lib
mkdir -p ./iconv/lib
mkdir -p ./iconv/include

cp ./iconv/iconv.h ./include/
cp ./iconv/iconv.h ./iconv/include/
cp ./iconv-build/iconv.lib ./iconv/lib/

ls -la ./include/iconv.h
ls -la ./iconv/include/iconv.h
ls -la ./iconv/lib/iconv.lib
exit

cd build
echo "CMake 配置 FreeTDS"
cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DENABLE_MSDBLIB=OFF -DENABLE_ODBC_WIDE=OFF -DBUILD_SHARED_LIBS=OFF -DWITH_OPENSSL=OFF -DCMAKE_INSTALL_PREFIX="E:\Github-Repositories\freetds\freetds_install" ..

wsl
grep -E "HAVE_ICONV|ICONV_CONST" ./include/config.h
exit

echo "开始编译 FreeTDS"
nmake

echo "安装 FreeTDS"
nmake install

