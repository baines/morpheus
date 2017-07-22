export MTX_URL="https://localhost:8448" # matrix server to connect clients to
export MTX_NAME="localhost" # what this server calls itself

if [[ $DEBUG ]]; then
	$DEBUG ./morpheus
else
	exec ./morpheus
fi
