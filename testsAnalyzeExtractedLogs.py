#!/usr/bin/env python3

import math
import sys
import csv
import os
from urllib.parse import urlparse
import simplejson as json

def getMedian(els, group):
    for el in els:
        if group in el["Group"]:
            return float(el["Median"].replace(',', ''))
    raise RuntimeError("Group not found: " + group)

def getGroups(els):
    ret = []
    for el in els:
        group_name = el["Group"].split('/')[-1]
        ret = ret + [group_name]
    return ret

def getOverhead(base, other):
    return str(round(base/other, 3))

def computeSummary(summaryFile, nameStock, nameOthers, parsedStock, parsedOthers):
    with open(summaryFile, "w") as f:
        writer = csv.writer(f)
        writer.writerow(["Benchmark", nameStock] + nameOthers)

        groups = getGroups(parsedStock)
        parsedAll = [parsedStock] + parsedOthers
        for group in groups:
            stock_val = getMedian(parsedStock, group)
            out_strs = []
            for parsed in parsedAll:
                other_val = getMedian(parsed, group)
                out_str = str(other_val) + " (" + getOverhead(other_val, stock_val) + ")"
                out_strs.append(out_str)

            writer.writerow([group] + out_strs)

def read(folder, filename):
    inputFileName1 = os.path.join(folder, filename)
    with open(inputFileName1) as f:
        input1 = f.read()
    return input1

def remove_suffix(word, suffix):
   return word[:-len(suffix)] if word.endswith(suffix) else word

def main():
    if len(sys.argv) < 2:
        print("Expected " + sys.argv[0] + " inputFolderName")
        exit(1)
    inputFolderName = sys.argv[1]

    stock_name = "stock"
    suffix = "_terminal_analysis.json"
    stock_terminal_analysis_file = stock_name + suffix

    stock_terminal_analysis_file_found = False
    other_terminal_analysis_files = []

    for filename in os.listdir(inputFolderName):
        if filename == stock_terminal_analysis_file:
            stock_terminal_analysis_file_found = True
        elif filename.endswith(suffix):
            other_terminal_analysis_files.append(filename)

    if not stock_terminal_analysis_file_found:
        print("Could not find stock terminal analysis file: " + stock_terminal_analysis_file)

    parsedStock = json.loads(read(inputFolderName, stock_terminal_analysis_file))["data"]
    parsedOthers = []
    namesOthers = []
    for filename in other_terminal_analysis_files:
        parsedOthers.append(json.loads(read(inputFolderName, filename))["data"])
        namesOthers.append(remove_suffix(filename, suffix))

    outputFileName = os.path.join(inputFolderName, "compare_" + stock_terminal_analysis_file + ".dat")
    computeSummary(outputFileName, stock_name, namesOthers, parsedStock, parsedOthers)

main()
