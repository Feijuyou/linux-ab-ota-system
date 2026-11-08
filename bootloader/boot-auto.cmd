# RDK X3 自动 A/B 启动。TF p2 = A，p3 = B。
# BOOT_ORDER 和 BOOT_*_LEFT 使用 RAUC 的 U-Boot 接口。
# 确认环境存储后，才用 ab_auto_save=yes 开启跨重启计数。

if test -z "${BOOT_ORDER}"; then
    setenv BOOT_ORDER "B A"
fi
if test -z "${BOOT_A_LEFT}"; then
    setenv BOOT_A_LEFT 3
fi
if test -z "${BOOT_B_LEFT}"; then
    setenv BOOT_B_LEFT 3
fi

# 沿用已验证的板型设备树和内存地址。缺少时先返回串口。
if test -z "${fdtfile}" ||
 test -z "${fdt_addr_r}" || 
 test -z "${kernel_addr_r}"; then
    echo "AB ERROR: missing fdtfile, fdt_addr_r or kernel_addr_r"
    exit 1
fi
if mmc dev 2 && mmc rescan; then
    echo "AB: TF card ready"
else
    echo "AB ERROR: cannot read TF card on MMC 2"
    exit 1
fi

for ab_slot in ${BOOT_ORDER}; do
    # 每次重新选分区。次数为 0 的槽不能重新得到尝试机会。
    setenv ab_part
    # 旧版 U-Boot 的 test 按十进制解析数字；用 setexpr 统一为十六进制。
    if test "${ab_slot}" = "A"; then
        if setexpr ab_left ${BOOT_A_LEFT} + 0 && test "${ab_left}" != "0" && setexpr BOOT_A_LEFT ${ab_left} - 1; then
            setenv ab_part 2
        fi
    elif test "${ab_slot}" = "B"; then
        if setexpr ab_left ${BOOT_B_LEFT} + 0 && test "${ab_left}" != "0" && setexpr BOOT_B_LEFT ${ab_left} - 1; then
            setenv ab_part 3
        fi
    fi

    if test -n "${ab_part}"; then
        echo "AB: trying ${ab_slot}, A left=${BOOT_A_LEFT}, B left=${BOOT_B_LEFT}"

        # 先保存扣减结果，Linux 卡住或被重启时仍能记住这次尝试。
        if test "${ab_auto_save}" = "yes"; then
            if saveenv; then
                echo "AB: boot attempts saved"
            else
                echo "AB ERROR: saveenv failed; returning to U-Boot"
                exit 1
            fi
        else
            echo "AB: trial mode; boot attempts only in RAM"
        fi

        # 复用手动版的启动参数及文件路径。板测使用基础设备树。
        setenv bootargs "earlycon loglevel=8 kgdboc=ttyS0 console=ttyS0,921600 video=hobot:x3sdb-hdmi rootfstype=ext4 rw rootwait root=/dev/mmcblk2p${ab_part} ubi.mtd=2,2048 mtdparts=hr_nand.0:6291456@0x0(miniboot),2097152@0x600000(env) rauc.slot=${ab_slot}"
        if ext4load mmc 2:${ab_part} ${fdt_addr_r} /boot/hobot/${fdtfile} && ext4load mmc 2:${ab_part} ${kernel_addr_r} /boot/Image; then
            booti ${kernel_addr_r} - ${fdt_addr_r}
            echo "AB ERROR: booti returned for slot ${ab_slot}"
        else
            echo "AB ERROR: cannot load DTB or Image from slot ${ab_slot}"
        fi
    fi
done

# 两个槽都不能启动时，保留计数供串口排查。
echo "AB ERROR: no slot booted; check BOOT_ORDER and BOOT_*_LEFT"
exit 1
