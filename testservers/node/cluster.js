const net = require('net');
const cluster = require('cluster')
const numCPUs = require('os').cpus().length


const start = async function startServer() {

    if (cluster.isMaster) {
        console.log(`Node JS cluster test echo server.`)

        for (let i = 0; i < numCPUs; i += 1) {
            cluster.fork()
        }

    } else {
		net.createServer(function(socket){
		    socket.on('data', function(data){
		        socket.write(data.toString())
		    });
		}).listen(7777);
    }
}

start()