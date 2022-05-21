Note: the namespace of QoZ has been changed to QoZ from SZ. Please modify your codes when using the APIs.

QoZ: Dynamic Quality Metric Oriented Error Bounded Lossy Compression for Scientific Datasets

## Introduction

This is the source code of QoZ: Dynamic Quality Metric Oriented Error Bounded Lossy Compression for Scientific Datasets

## Dependencies

Please Installing the following dependencies before running the artiact evaluation experiments:

* Python >= 3.6
* numpy 
* pandas 
* qcat (from https://github.com/Meso272/qcat, check its readme for installation guides. Make sure the following executables are successfully installed: calculateSSIM and computeErrAutoCorrelation)

## 3rd party libraries/tools

* Zstandard (https://facebook.github.io/zstd/). Not mandatory to be mannually installed as Zstandard v1.4.5 is included and will be used if libzstd can not be found by
  pkg-config.

## Installation

* mkdir build && cd build
* cmake -DCMAKE_INSTALL_PREFIX:PATH=[INSTALL_DIR] ..
* make
* make install

Then, you'll find all the executables in [INSTALL_DIR]/bin and header files in [INSTALL_DIR]/include

## Single compression/decompression testing Examples

You can use the executable 'qoz' command to do the compression/decompression. Just run "qoz" command to check the instructions for its arguments.
Currently you need to add a configuration file to the argument line (-c) for activating the new features of QoZ. 
The corresponding cofiguration files for each test dataset can be generated by generate_config.py (details shown on following).