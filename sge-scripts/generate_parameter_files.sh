#!/bin/bash

roms_path=$1
suffix=$2

for fs in 05 10 20 30; do
  for b in inf; do
    for f in 0 1 2 3; do
      ./generate_single_parameter_files.sh $fs $b $f "--nodisplay" $roms_path $roms_path/README > parameters/fs=$fs.budget=$b.features=$f.$suffix.txt;
    done;
  done;
done

for fs in 05 10 20 30; do
  for b in 0.5 1.0 2.0 4.0 8.0 16.0; do
    for f in 0 1 2 3; do
      ./generate_single_parameter_files.sh $fs $b $f "--execute-single-action,--nodisplay" $roms_path $roms_path/README > parameters/fs=$fs.budget=$b.features=$f.$suffix.txt;
    done;
  done;
done

