# EquinoxOS ~/.bashrc — coreutils через busybox (Этап 8).
# ВАЖНО: без \-переносов строк — git на Windows может дать файлу CRLF,
# и тогда `\` + CR ломает синтаксис. Одна строка списка = надёжно.
EQ_APPLETS="ls cat grep egrep fgrep head tail tr wc sort uniq cut uname mkdir rmdir rm cp mv touch basename dirname env seq sleep yes tee rev date whoami hostname du df sync clear xxd hexdump md5sum sha256sum expr which find sed awk cal free uptime"
for a in $EQ_APPLETS; do alias $a="/bin/busybox.elf $a"; done
unset a EQ_APPLETS
