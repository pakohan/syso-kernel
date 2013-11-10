#!/bin/sh
echo "Content-type: text/html"
echo ""
echo "<h2>Network</h2>"
ifconfig | sed 's/.$/<br>/g'