# pass in inputfilename and outputfilename
set terminal pdf enhanced size 3.5,1.2

set key outside
set key above
set key font "Times New Roman,10"

set style data histogram
set style histogram gap 3
set style fill solid border rgb "black"

set border 1

set xtics scale 0
set xtics font "Times New Roman,9"

set yrange [-45:80]
set ytics font "Times New Roman,10" -40,20,240
set ylabel "Overhead (%)"
set ylabel font "Times New Roman"

set arrow from -1,80 to 9,80 nohead
set arrow from -1,-45 to -1,80 nohead
set arrow from 9,-45 to 9,80 nohead
set arrow from -1,0 to 9,0 nohead lc rgb "#bbbbbb"
set grid ytics lt 0 lw 2 lc rgb "#bbbbbb"

set output outputfilename
set datafile separator ','

# 4 colors - #e66101 #fdb863 #b2abd2 #5e3c99
# 5 colors - #008837 #a6dba0 #f7f7f7 #c2a5cf #7b3294

plot inputfilename using 3:xtic(1) title col(3) linecolor rgb "#a6dba0" fill pattern 3, \
                '' using 4:xtic(1) title col(4) linecolor rgb "#f7f7f7" fill pattern 3, \
                '' using 5:xtic(1) title col(5) linecolor rgb "#c2a5cf" fill pattern 3, \
                '' using 6:xtic(1) title col(6) linecolor rgb "#7b3294" fill pattern 3