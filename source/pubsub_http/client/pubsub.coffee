# This is the CoffeeScript client library for the Sioux-PubSub-Server: 
# https://github.com/TorstenRobitzki/Sioux

global = `Function('return this')()`
global.PubSub = global.PubSub || {}

create_ajax_transport = ( url = '/pubsub' )->
    ( obj, cb, message_url = url )->
        $.ajax( 
            url: message_url,
            type: 'POST',
            contentType: 'application/json;charset=UTF-8',
            data: JSON.stringify obj )
        .success( ( data, textStatus, jqXHR )-> cb false, data ) 
        .error( ( jqXHR, textStatus )-> cb true )                 
        
class Node 
    constructor: ( @cb )->    
        @data = null
        @version = null

    update: ( update )->

        return false if update[ 'update' ] && @version != update.from

        @data = if update[ 'update' ] then PubSub.update( @data, update.update ) else update.data
        @version = update.version

        @cb( update.key, @data )

        true
                             
    error: ( key, error )->
        @cb( key, null, error )
                                     
class Impl
    constructor: ( @transport = create_ajax_transport() )->
        @subjects = new PubSub.NodeList
        clear_pendings.call @

        # if the session_id is null, there is currently no connection to the server
        @session_id = null
        @reconnecting = false
        
        # The number of outstanding responses 
        @current_transports = 0

    subscribe: ( node_name, callback )->
        @subjects.insert node_name, new Node callback
        @pending_subscriptions.push node_name 
        
        if !@reconnecting && ( @session_id || @current_transports == 0 )
            issue_request.call @

    unsubscribe: ( node_name )->
        unless @subjects.remove node_name
            throw 'not subscribed'

        if @session_id && !@reconnecting
            @pending_unsubscriptions.push node_name
            issue_request.call @ 
        
    publish: ( message, callback )->
        cb = ( error, response )->
            if callback
                if error
                    callback true
                else
                    callback false, response[ 0 ]
                                
        @transport( [ message ], cb, '/publish' )      
                  
    clear_pendings= ->
        @pending_subscriptions   = []
        @pending_unsubscriptions = []
        @pending_resubscriptions = []

    issue_request= ->
        cmds = for pending in @pending_subscriptions
            { subscribe: pending }

        for pending in @pending_unsubscriptions
            cmds.push { unsubscribe: pending }    

        for pending in @pending_resubscriptions
            cmds.push { unsubscribe: pending }
            cmds.push { subscribe: pending }

        clear_pendings.call @

        payload = if @session_id
            if cmds.length then { id: @session_id, cmd: cmds } else { id: @session_id }
        else 
            { cmd: cmds }     

        @current_transports++     
        @transport payload, ( error, response ) => 
            receive_call_back.call @, error, response
                    
    handle_response= ( resp )->          
        if resp.subscribe?
            msg = resp.subscribe
            node = @subjects.get( msg )
            node.error msg, resp.error
           
            @subjects.remove msg
            
    handle_update= ( update )->
        node = @subjects.get( update.key )

        if node && !node.update update
            # resubscribe because of a version missmatch
            @subjects.remove update.key
            @pending_resubscriptions.push update.key
                                                        
    receive_call_back= ( error, response )->
        @current_transports--

        if error 
            disconnect.call @, true
        else 
            if @session_id && @session_id != response.id
                disconnect.call @, false, response.id
            else
                @session_id = response.id
                
                handle_response.call @, resp for resp in response.resp if response.resp?
                handle_update.call @, update for update in response.update if response.update?
                    
                if @current_transports == 0 && !@reconnecting
                    issue_request.call @
        
    disconnect= ( wait_before_reconnect, new_session = null )->
        @session_id = new_session
        @reconnecting = true 
        
        if @current_transports == 0
            if wait_before_reconnect
                setTimeout => 
                    try_reconnect.call @
                , 30000
            else
                try_reconnect.call @                            
         
    try_reconnect= ->
        @reconnecting = false

        clear_pendings.call @
        
        @subjects.each ( name ) => @pending_subscriptions.push name
        issue_request.call @

impl = null

# resets the internal state of the library. This function is mainly intended for testing.
PubSub.reset = ->
    impl = new Impl

# Subscribes to the node given by node_name. The given callback will be called, when the subscribed data is received,
# updated or if there is an error with the subscription. The callback have to take three arguments:
#  node_name : the name of the subscribed node
#  data      : the initial or updated data of the subscribed node. This is only valid, if the error parameter is undefined
#  error     : If not undefined, this indicates the reason, why the subscription failed. For now, there can be two reasons:
#              'invalid' which indicates, that there is no such node and 'not allowed', which indicates, that you are not
#              allowed (authorized) to subscribe to the given node. If an error was indicated, the callback will not be 
#              called again.
# 
# The behaviour of subscribing more than once to the same subject is undefined and should be avoided.

PubSub.subscribe = ( node_name, callback )->
    impl ?= new Impl
    
    if arguments.length != 2
        throw 'wrong number of arguments to PubSub.subscribe'
      
    impl.subscribe node_name, callback      
                 
# Removes the subscription to the given node. No further updates to the node will be notified.                 
PubSub.unsubscribe = ( node_name )->
    if arguments.length != 1
        throw 'wrong number of arguments to PubSub.unsubscribe'

    impl.unsubscribe node_name

# Transmit a user defined message to the server. message can be anything as long as it's json encodeable. The function
# will generate a http post request to /publish with the given message as json encoded body. The callback will take 
# two arguments, the first is a boolean indicating an error and the second is the response from the server, if no error
# occured. The callback argument is optional.
PubSub.publish = ( message, callback )->
    impl ?= new Impl    

    if arguments.length != 2 && arguments.length != 1
        throw 'wrong number of arguments to PubSub.publish'
      
    impl.publish message, callback      

# Sets an ajax transport function. That function takes 2 arguments, the first 
# argument is an object to be transported. The second argument is a callback takeing a boolean. That boolean indicates
# an error if set to true. The second parameter to that callback is the answer from the server. The callback will called, 
# when the http request succeded or failed. 
PubSub.configure_transport = ( new_transport )->
    impl = new Impl( new_transport ? create_ajax_transport() )   
