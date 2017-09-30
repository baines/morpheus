export MTX_URL="https://localhost:8448" # matrix server to connect clients to
export MTX_LISTEN_PORT="1999" # where the "IRCd" will listen

if [[ $DEBUG ]]; then
	$DEBUG ./morpheus
else
	exec ./morpheus
fi
