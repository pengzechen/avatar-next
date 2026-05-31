


# 快速验证代码方法


## 第一步清理

make clean

## 第二步编译

根据情况你可以自己选自使用什么日志等级。

make PLATFORM=rk3588-aarch64 kernel LOG=<level> SMP=8 -j8 

如果编译不通过，那你改代码，改完代码回到步骤一，再往下走。

## 第三步拷贝到指定目录

cp build/kernel_aarch64.bin imgs/

## 第四步打开串口并重启

res.txt 我希望放在项目根目录，也方便我查看。

stty -F /dev/ttyUSB0 1500000 raw -echo
cat /dev/ttyUSB0 > res.txt &
/home/ajax/Proj/OS/Thread-Process-Lock/avatar/tools/power/miplug off && sleep 3 && /home/ajax/Proj/OS/Thread-Process-Lock/avatar/tools/power/miplug on 

## 第五步查看运行情况

你可以根据运行情况自己设定等待时间 至少15s

sleep <second>


## 第六步停止

pkill cat
pkill -9 minicom

## 第七步查看情况

查看情况
tail -<lines> res.txt

注意你要查看编译时间确保每次执行的代码是最新的代码。