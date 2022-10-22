# pass in inputfilename and outputfilename
set terminal pdf enhanced size 3.5,1.2

set key outside
set key above
set key font "Times New Roman,10"

set style data histogram
set style histogram cluster gap 3
set style fill solid border rgb "black"

set border 1
set boxwidth 0.5

set xtics scale 0
set xtics font "Times New Roman,9"

set yrange [0:160]
set ytics font "Times New Roman,10" 0,25,340
set ylabel "Norm. runtime"
set ytics format "%g%%"
set ylabel font "Times New Roman"

set arrow from -1,160 to 9,160 nohead
set arrow from -1,-5 to -1,160 nohead
set arrow from 9,-5 to 9,160 nohead
set arrow from -1,0 to 9,0 nohead lc rgb "#bbbbbb"
set arrow from -1,100 to 9,100 nohead lc rgb "#aa000000" back
set grid ytics lt 0 lw 2 lc rgb "#bbbbbb"

set output outputfilename
set datafile separator ','

plot inputfilename using 3:xtic(1) title col(3) linecolor rgb "#D7191C" fill pattern 3, \
		'' using 2:xtic(1) title col(2) linecolor rgb "#FFFFBF" fill pattern 3, \
                '' using 6:xtic(1) title col(6) linecolor rgb "#2B83BA" fill pattern 3
