# Simple Block Device Driver
Implementation of Linux kernel 6.8.X simple block device.

## Build
`make`

## References
- [Linux Device Drivers](https://lwn.net/Kernel/LDD3/)
- [Linux Kernel Development](https://rlove.org)
- [Linux Kernel Teaching](https://linux-kernel-labs.github.io/refs/heads/master/labs/block_device_drivers.html)
- [Linux Kernel Sources](https://github.com/torvalds/linux)

## Инструкция по использованию

После сборки модуля ядра его можно запустить либо с параметрами, либо без:
1. ```sudo insmod sbdd.ko``` - будет создан /dev/sbdd размером 100Мб в RAM
2. ```sudo insmod sbdd.ko target_bdev_name="/dev/blockdev" redirect=true``` - будет создан /dev/sbdd ссылающийся на /dev/blockdev.

Например, подключим флешку. Она отобразится в Linux как /dev/sda. Теперь введем:
sudo insmod sbdd.ko target_bdev_name="/dev/sda" redirect=true. В системе появится новый блочник /dev/sbdd, который будет 
пробрасывать все запросы на /dev/sda.

Кроме того, мы можем привязывать "внешние" целевые блочные устройства уже после загрузки драйвера.
Для этого в sysfs зарегистрированы 2 параметра - target_bdev_name и redirect (находятся в /sys/module/sbdd/parameter/).
Привяжем свое устройство:
1. ```sudo insmod sbdd.ko capacity_mib=50``` - видим, что появился /dev/sbdd размером 50Мб 
2. ```sudo dd if=/dev/zero of=/dev/ram0 count=500 bs=1M``` - аллоцируем память в RAM под наше устройство
3. ```sudo losetup --find --show /dev/ram0``` - регистрируем его как loopback device (например, /dev/loop13)
4. ```printf "/dev/loop13" > /sys/module/sbdd/parameters/target_bdev_name``` - сообщаем драйверу имя нового блочника
5. ```echo true > /sys/module/sbdd/parameters/redirect``` - перенаправляем данные на наш /dev/loop13 
После всех действий lsblk покажет, что теперь /dev/sbdd занимает 500Мб, что ожидаемо.
Если же мы хотим работать с первоначальным RAM диском (который аллоцировало ядро), то достаточно набрать:
1. ```echo false > /sys/module/sbdd/parameters/redirect```
После этого увидим, что размер /dev/sbdd снова 50Мб.

Тестирование проводилось самое примитивное: записывались блоки данных по случайным смещениям и со случайными размерами
на /dev/sbdd. Затем читаем эти области памяти (из /dev/sbdd) и сравниваем с исходными данными. Если diff пропускает, то тест 
пройден. Вот пример тестирования для "проброски" запросов на внешнюю флэшку /dev/sda:
1. ```sudo insmod sbdd.ko```
2. ```printf "/dev/sda" > /sys/module/sbdd/parameters/target_bdev_name```
3. ```echo true > /sys/module/sbdd/parameters/redirect```

Для чистоты эксперимента, будем обходить страничный кэш. Чтобы постоянно не набирать
```echo 3 > /proc/sys/vm/dropcaches```, просто будем использовать флаги oflag=direct и iflag=direct.

1-й тест:
```
sudo dd if=/dev/random of=./rand_data.txt count=2000 bs=1M oflag=direct status=progress
sudo dd if=./rand_data.txt of=/dev/sbdd count=2000 bs=1M oflag=direct status=progress
sudo dd if=/dev/sbdd of=./sbdd_text.txt count=2000 bs=1M iflag=direct status=progress
sudo dd if=/dev/sda of=./sda_text.txt count=2000 bs=1M iflag=direct status=progress (для проверки, что данные пишутся в /dev/sda)
diff sbdd_text.txt rand_data.txt
diff sda_text.txt rand_data.txt
```

2-й тест:
```
sudo dd if=/dev/random of=./rand_seek_data.txt count=100 bs=1M oflag=direct status=progress
sudo dd if=./rand_seek_data.txt of=./rand_data.txt count=100 bs=1M seek=25 oflag=direct conv=notrunc status=progress
sudo dd if=./rand_seek_data.txt of=/dev/sbdd count=100 bs=1M seek=25 oflag=direct status=progress
sudo dd if=/dev/sbdd of=./sbdd_text.txt count=2000 bs=1M iflag=direct status=progress
sudo dd if=/dev/sda of=./sda_text.txt count=2000 bs=1M iflag=direct status=progress (для проверки, что данные пишутся в /dev/sda)
diff sbdd_text.txt rand_data.txt
diff sda_text.txt rand_data.txt
```

3-й тест:
```
sudo dd if=/dev/random of=./rand_seek_data_th_1.txt count=50 bs=1M oflag=direct status=progress
sudo dd if=/dev/random of=./rand_seek_data_th_2.txt count=50 bs=1M oflag=direct status=progress
sudo dd if=./rand_seek_data_th_1.txt of=./rand_data.txt count=50 bs=1M seek=500 oflag=direct conv=notrunc status=progress
sudo dd if=./rand_seek_data_th_2.txt of=./rand_data.txt count=50 bs=1M seek=700 oflag=direct conv=notrunc status=progress
```
В двух параллельных терминалах:
1. Терминал 1: ```sudo dd if=./rand_seek_data_th_1.txt of=/dev/sbdd count=50 bs=1M seek=500 oflag=direct status=progress```
2. Терминал 2: ```sudo dd if=./rand_seek_data_th_2.txt of=/dev/sbdd count=50 bs=1M seek=700 oflag=direct status=progress```
```
sudo dd if=/dev/sbdd of=./sbdd_text.txt count=2000 bs=1M iflag=direct status=progress
sudo dd if=/dev/sda of=./sda_text.txt count=2000 bs=1M iflag=direct status=progress
diff sbdd_text.txt rand_data.txt
diff sda_text.txt rand_data.txt
```

**P.S. Сборка и тестирование производилось на Ubuntu 22.04 с ядром 6.8.0-90-generic**