require 'eventmachine'

module EchoServer
 def receive_data data
   send_data data
 end
end

EventMachine.run {
  EventMachine.start_server "0.0.0.0", 7777, EchoServer
}
