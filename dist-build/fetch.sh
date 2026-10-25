# Pulled in by the other dist-build scripts.

DL=`pwd`/build/dl

download() {
	if command -v curl > /dev/null; then
		curl -fSL --retry 3 --progress-bar -o "$2" "$1"
	elif command -v wget > /dev/null; then
		wget -q --show-progress -O "$2" "$1"
	else
		echo "need curl or wget to fetch $1"
		exit 1
	fi
}

# checksum <file> <expected>: the hash of <file>, of the same kind
# as <expected> (40 characters is sha1, otherwise sha256).
checksum() {
	if [ ${#2} -eq 40 ]; then
		if command -v sha1sum > /dev/null; then
			sha1sum "$1" | cut -d' ' -f1
		else
			shasum -a 1 "$1" | cut -d' ' -f1
		fi
	else
		if command -v sha256sum > /dev/null; then
			sha256sum "$1" | cut -d' ' -f1
		else
			shasum -a 256 "$1" | cut -d' ' -f1
		fi
	fi
}

# fetch <name> <url> <hash> <archive> <dir>
#
# Fetch <url> as build/dl/<archive>, verify it and unpack it into
# <dir>/<name>, the archive must hold a single top level directory.
# Nothing is done if <dir>/<name> already holds this exact archive.
fetch() {
	name=$1
	url=$2
	want=$3
	archive=$DL/$4
	dir=$5

	if [ -f $dir/$name/.vigil-fetched ] &&
	    [ "`cat $dir/$name/.vigil-fetched`" = "$want" ]; then
		return
	fi

	mkdir -p $DL $dir

	if [ ! -f $archive ] || [ "`checksum $archive $want`" != "$want" ]; then
		echo "fetching $name from $url"
		rm -f $archive.tmp
		download $url $archive.tmp
		mv $archive.tmp $archive
	fi

	got=`checksum $archive $want`
	if [ "$got" != "$want" ]; then
		echo "checksum mismatch for $archive"
		echo "  expected $want"
		echo "  got      $got"
		rm -f $archive
		exit 1
	fi

	echo "unpacking $name into $dir/$name"

	rm -rf $dir/$name $dir/$name.tmp
	mkdir -p $dir/$name.tmp

	case $archive in
	*.zip)
		if ! command -v unzip > /dev/null; then
			echo "need unzip to unpack $archive"
			exit 1
		fi
		unzip -q $archive -d $dir/$name.tmp
		;;
	*)
		tar -xf $archive -C $dir/$name.tmp
		;;
	esac

	mv $dir/$name.tmp/* $dir/$name
	rm -rf $dir/$name.tmp

	echo "$want" > $dir/$name/.vigil-fetched
}
