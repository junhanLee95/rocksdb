#!/bin/bash

CEPH_HOME="/home/ceph/ceph"

export CEPH_HOME="${CEPH_HOME}"
#export JAVA_HOME="/usr/lib/jvm/java-1.8.0-openjdk/"
export JAVA_HOME="/usr/lib/jvm/java-openjdk/"

#make bluefs_test -j32 
#exit 0

if [ ! -f "java/test-libs/assertj-core-1.7.1.jar" ]; then
	wget https://repo1.maven.org/maven2/org/assertj/assertj-core/1.7.1/assertj-core-1.7.1.jar
	mv assertj-core-1.7.1.jar java/test-libs
	echo "[COMPILE PROCESS] Download the additional files"
fi

make rocksdbjavastaticrelease -j32 
