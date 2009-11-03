<?php
$num = 10;
for ($i = 1; $i < $num; $i++){
	send($i);
}



function send($i){
	$fp = fsockopen("udp://127.0.0.1", 12345, $errno, $errstr);
	if (!$fp) {
		die("ERROR: $errno - $errstr<br />\n");
	}
	echo $i , "\n";
	$str = (string) $i;
	fwrite($fp, $str);
	fclose($fp);
}

?>