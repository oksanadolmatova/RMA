#!/bin/bash
case $# in
0)
    # figure out which distribution this is
    if [ -f /etc/lsb-release ]; then
	source /etc/lsb-release
	SUITE=$DISTRIB_CODENAME
    elif [ -f /etc/os-release ]; then
	source /etc/os-release
	if [[ $ID == debian ]]; then
	    SUITE="${VERSION#*(}"
	    SUITE="${SUITE%)*}"
	else
	    case $VERSION in
	    *\(*\)*)
		SUITE="${VERSION#*(}"
		SUITE="${SUITE% *)*}"
		;;
	    *)
		SUITE="${VERSION#*, }"
		SUITE="${SUITE% *)*}"
		;;
	    esac
	    SUITE=${SUITE,,}
	fi
    else
	echo "don't know what distro this is"
	exit 1
    fi
    ;;
1)
    # we're passed the name of the distribution
    SUITE=$1
    ;;
esac

case $SUITE in
wheezy | trusty)
    # fix control file because these systems don't have liblas and a
    # too old version of libgeos
    sed -i -e 's/, libgeos-dev[^,]*//;s/, liblas-c-dev[^,]*//' \
	-e '/^Package:.*lidar/,/^$/d' \
	-e '/^Package:.*geom/,/^$/d' debian/control
    rm debian/libmonetdb5-server-lidar.install debian/libmonetdb5-server-geom.install
    sed -i '/geo[ms]=yes/s/yes/no/;/gdal=yes/s/yes/no/;/lidar=yes/s/yes/no/;/liblas=yes/s/yes/no/' debian/rules
    ;;
esac

case $SUITE in
wheezy | jessie | trusty | wily)
    # Xenial Xerus (and presumably newer releases) uses php-cli,
    # all others still have php5-cli and don't have php*-sockets
    sed -i 's/php-cli/php5-cli/;s/, *php-sockets//' debian/control
    ;;
esac

case $SUITE in
wheezy)
    # numpy is too old
    sed -i -e 's/, python-dev[^,]*//;s/, python-numpy[^,]*//' \
	-e '/^Package:.*monetdb-python2/,/^$/d' debian/control
    sed -i '/pyintegration=yes/s/yes/no/' debian/rules
    rm debian/monetdb-python2.install
    ;;
trusty)
    # the trusty linker produces unresolved references to openSSL functions
    sed -i '/openssl_LIBS/s/WIN32?//' clients/mapilib/Makefile.ag
    lib=$(grep openssl_LIBS clients/mapilib/Makefile.am)
    lib="${lib%% *}"
    sed -i "s/\\\$($lib)/\$(openssl_LIBS)/g" clients/mapilib/Makefile.am clients/mapilib/Makefile.in
    ;;
esac
