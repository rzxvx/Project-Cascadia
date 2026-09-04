python3 fix_adler32.py output/kernel.complzss output/adler_variants/

sleep 4

cd ~/Desktop/ipad-mini-linux/ibootfiles

./img3maker -f ../output/adler_variants/adler_unc_1.complzss \
    -t krnl -s s5l8942x \
    -o ../output/ADLER_DCSD/adler_unc_1.img3
