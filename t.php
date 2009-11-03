<?php
$host = 'localhost';
$port = 11311;
$len = strlen($buf);
$socket = socket_create(AF_INET, SOCK_DGRAM, SOL_UDP);
socket_set_nonblock($socket);
for ($i = 1; $i < 20; $i++) {
	$buf = sprintf("%d", $i);
	$len = strlen($buf);
	if (($n = @socket_sendto($socket, $buf, $len, 0, $host, $port)) != $len) {
		echo $i." send error: ".$n."\n";
		exit;
	} else {
		echo $i." sent: ".$buf."\n";
	}
}

socket_close($socket);
?>
