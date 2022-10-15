#!/usr/bin/env python3
import csv
import re
import sys

def main():
    if len(sys.argv) < 3:
        print("Expected " + sys.argv[0] + " inputFile outputFile")
        exit(1)

    inputFile = sys.argv[1]
    outputFile = sys.argv[2]
    lines = []
    with open(inputFile, mode ='r')as f:
        csvread = csv.reader(f)
        lines = [line for line in csvread]

    groups = lines[0][1:]
    heading_row = ['Image'] + groups

    with open(outputFile, 'w') as f:
        writer = csv.writer(f)
        writer.writerow(heading_row)

        for line in lines[1:]:
            img_name = line[0]
            col_heading = ''

            for img_class in ['best', 'none', 'default']:
                if img_class in img_name:
                    col_heading += img_class

            col_heading += '\\n'

            resolution = re.search(r'_\d+\.((jpeg)|(png)).*', line[0]).group().split('.')[0].split('_')[1]
            col_heading += resolution + 'p'

            out_row = [col_heading]

            for data in line[1:]:
                colval = (float(data.split('(')[1].split(')')[0]) - 1) * 100
                out_row += [colval]

            writer.writerow(out_row)

main()