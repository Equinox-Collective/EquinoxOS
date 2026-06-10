# EquinoxOS ~/.bashrc — coreutils через busybox (Этап 8).
# Алиасы раскрываются и внутри пайпов интерактивного bash:
#   ls /bin | grep elf
for a in ls cat grep egrep fgrep head tail tr wc sort uniq cut uname \
         mkdir rmdir rm cp mv touch basename dirname env seq sleep yes \
         tee rev date whoami hostname du df sync clear xxd hexdump \
         md5sum sha256sum expr which find sed awk cal free uptime; do
  alias $a="/bin/busybox.elf $a"
done
unset a
