const net = require('net');

console.log(`Node JS test echo server.`)

net.createServer(function(socket){
    socket.on('data', function(data){
        socket.write(data.toString())
    });
}).listen(7777);