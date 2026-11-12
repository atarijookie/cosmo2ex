/*================================================================
  CosmosEx simple remote control
  ================================================================
  Just a quick usage example for the CosmosEx HTTP Mouse/Keybard API
  sends keyboard events as linux keycodes to the HTTP API as well
  as relative mouse packets
  ----------------------------------------------------------------
  RESTlike interface, using json

  /hid/mouse 	POST
	Mousemove: 
	{ "type":"relative", "x": 8, "y": 21 }
	Mouse click:
	{ "type": "buttonleft", "state": "down" }
	{ "type": "buttonright", "state": "up" }

  /hid/keyboard 	POST 
	{ "type": "pc", "code": 40, "state": "down" };
	{ "type": "pc", "code": 40, "state": "up" };
	
	(keycodes are linux keycodes, can be converted from browser keycodes via array in keylinux.js)

  ----------------------------------------------------------------*/
//TODO: 
//		ANDROID:
//		- Chrome 31 on android does always send code 0 on key events (see https://code.google.com/p/chromium/issues/detail?id=118639)
//		  at least Chrome 35 is needed, but even then certain special keys won't work....
// 		POINTERLOCK:
// 		- when pointer is locked, the user needs an alternative for the ESC key on native keyboard, which is unusable in that mode (as it leaves pointer lock mode)
// 		ST KEYBOARD:
//		- st keyboard on android lets the mouse move - event bubble up for some odd reason
//		- multitouch for st keyboard
//		- send st keycodes
//    TOUCH DEVICES:
//    - mouse drag
//    - better orientation support

var CosmosEx = CosmosEx || {};
CosmosEx.Remote=function(){
    	//Mouse position states
    	var iMouseX=null;
    	var iMouseY=null;
    	var iTouchMouseX=null;
    	var iTouchMouseY=null;
    	var iCurrentDX=null;
    	var iCurrentDY=null;
    	//relative mouse speed multiplier (1 = unchanged), set from remote.html slider
    	var fMouseSpeed=1;
    	//fractional parts left after scaling - carried to the next move, so slow speeds don't lose small moves
    	var fRestDX=0;
    	var fRestDY=0;
    	//touch devices keyboard status
    	var bShowKeyboard=false;
    	//Help display status
    	var bShowHelp=false;
    	//ST imagemap-keyboard display state
    	var bShowStKeyboard=false;
    	//a simple "queue" - gather only one mouse delta per timeslice
    	var currentMouseMove=null;
    	//mouse move POSTs not finished yet - next move is sent only when all are done (moves meanwhile add up), so
    	//requests don't pile up / come out of order on a slow connection
    	var iMouseRequestsInFlight=0;
    	//helper canvas for pointerlock (grabbing the pointer in the window), only works on Chrome/FF/Safari/IE, not used on touch devices 
    	var canvas=null;
    	//pointerlock states
    	var bHasPointerLock=false;
    	var bPointerLocked=null;
    	var iLastSendKeyDown=null;
    	/* ==========================
    	 * 			private functions
    	 */
    	var onMouseMove=function(e){
				//console.log("mousemove");
				//console.log(e);
				//with grab support, ignore mouse moves until 'Grab mouse' is active - otherwise ST mouse moves while just using the webpage
				if( bHasPointerLock && !bPointerLocked ){
					iMouseX=null;
					iMouseY=null;
					currentMouseMove=null;
					return;
				}
				//store initial position
				if( iMouseX==null && iMouseY==null ){
					iMouseX=e.clientX;
					iMouseY=e.clientY;
				}
				//add this delta to the 1-entry queue. will be dscarded when mouse is moved further, the delta is recalculated until it is send
				(function(){
					var iDX=e.clientX-iMouseX;
					var iDY=e.clientY-iMouseY;
					//we might have had succes locking the pointer onto the canvas
					if( bPointerLocked ){
						//use the delta values given, but use a sum - the fixed sending time slice would otherwise miss deltas
						iCurrentDX+=e.movementX ||
			                		e.mozMovementX ||
			                		e.webkitMovementX ||
                					0;
						iCurrentDY+=e.movementY ||
					                e.mozMovementY ||
					                e.webkitMovementY ||
		                			0;
		               	iDX=iCurrentDX;
		               	iDY=iCurrentDY;
					}
					var iX=e.clientX;
					var iY=e.clientY;
					currentMouseMove=function(){
						sendMouseRelative(iDX,iDY)
						//update last position with the one we send
						iMouseX=iX;
						iMouseY=iY;
						iCurrentDX=0;
						iCurrentDY=0;
					};
				})();
			};
		var onTouchStart=function(e){
				//console.log("onTouchStart");			
				//currentMouseMove=null;
				iTouchMouseX=null;
				iTouchMouseY=null;
			};
		var onTouchEnd=function(e){
				//console.log("onTouchEnd");			
				//currentMouseMove=null;
				iTouchMouseX=null;
				iTouchMouseY=null;
			};
		var onTouchMove=function(e){
				currentMouseMove=null;
				e.preventDefault();
				var touch = e.touches[0]; // || e.changedTouches[0];
				//store initial position
				if( iTouchMouseX==null && iTouchMouseY==null ){
					iTouchMouseX=touch.pageX;
					iTouchMouseY=touch.pageY;
				}
				(function(){
					var iDX=touch.pageX-iTouchMouseX;
					var iDY=touch.pageY-iTouchMouseY;
					var iX=touch.pageX;
					var iY=touch.pageY;
					currentMouseMove=function(){
						//update last position with the one we are sending
						iTouchMouseX=iX;
						iTouchMouseY=iY;
						sendMouseRelative(iDX,iDY)
					};
				})();

			};
		var sendMouseRelative=function(iDX,iDY){
				//apply the speed multiplier, keep the fraction for the next move
				if( fMouseSpeed!=1 ){
					var fDX=iDX*fMouseSpeed+fRestDX;
					var fDY=iDY*fMouseSpeed+fRestDY;
					iDX=fDX<0 ? Math.ceil(fDX) : Math.floor(fDX);
					iDY=fDY<0 ? Math.ceil(fDY) : Math.floor(fDY);
					fRestDX=fDX-iDX;
					fRestDY=fDY-iDY;
				}
				var transmit=function(iDX,iDY){
					$.ajax({
						type: 'POST',
						url: '/hid/mouse',
						data: JSON.stringify({ "type":"relative", "x": iDX, "y": iDY }),
						contentType: 'application/json',
						complete: function(){
							iMouseRequestsInFlight=Math.max(0, iMouseRequestsInFlight-1);
						}
					});
					iMouseRequestsInFlight++;
				}
				//we only can transmit movement distance of -128 to 127 in one go, so slice this into several packets if necessary
				if( !(iDX>=-128 && iDX<128 && iDY>=-128 && iDY<128) ){
					//slice DX
					while(iDX>127 || iDX<-128){
						//depending on delta negative or positive, transmit maximum packet until we reach a value lower than the maximum trnsmittable value
						if( iDX>=0 ){
							transmit(127,0);
							iDX-=127
						}else{
							transmit(-128,0);
							iDX+=128
						}
					}
					//slice DY
					while(iDY>127 || iDY<-128){
						if( iDY>=0 ){
							transmit(0,127);
							iDY-=127
						}else{
							transmit(0,-128);
							iDY+=128
						}
					}
				}
				//transmit rest if any movement left
				if( iDX!=0 || iDY!=0 ){
					transmit(iDX,iDY);
				}

			};	
    	var onMouseDown=function(e){
    			if( e.which==1 ){
    				e.preventDefault();
	    			sendMouseKey("buttonleft","down");
	    			return false;
    			}
    			if( e.which==3 ){
    				e.preventDefault();
	    			sendMouseKey("buttonright","down");
	    			return false;
    			}
			};
    	var onMouseUp=function(e){
    			if( e.which==1 ){
    				e.preventDefault();
	    			sendMouseKey("buttonleft","up");
	    			return false;
    			}
    			if( e.which==3 ){
    				e.preventDefault();
	    			sendMouseKey("buttonright","up");
	    			return false;
    			}
			};
		//touch device - double tap triggers mouse click
    	var onDoubleTap=function(e){
    			sendMouseKey("buttonleft","down");
    			setTimeout(function(){sendMouseKey("buttonleft","up")},100);
			};
		//touch device - double tap with 2 fingers triggers double click
		var onDoubleTap2Fingers=function(e){
    			sendMouseKey("buttonleft","down");
    			setTimeout(function(){
    				sendMouseKey("buttonleft","up")
	    			setTimeout(function(){
	    				sendMouseKey("buttonleft","down")
		    			setTimeout(function(){
		    				sendMouseKey("buttonleft","up")
		    			},100);
	    			},100);
    			},100);
			};
	    var sendMouseKey=function(sType,sState){
				$.ajax({
					type: 'POST',
					url: '/hid/mouse',
					data: JSON.stringify({ "type": sType, "state": sState }),
					contentType: 'application/json'
				});
			};
    	//physical key position (KeyboardEvent.code) to linux keycode - independent of the keyboard layout set in the browser / OS,
    	//the ST then applies its own layout (like a real keyboard would). Keys not listed here are not sent.
    	var codelinux={
    		Escape:1, Digit1:2, Digit2:3, Digit3:4, Digit4:5, Digit5:6, Digit6:7, Digit7:8, Digit8:9, Digit9:10, Digit0:11,
    		Minus:12, Equal:13, Backspace:14, Tab:15,
    		KeyQ:16, KeyW:17, KeyE:18, KeyR:19, KeyT:20, KeyY:21, KeyU:22, KeyI:23, KeyO:24, KeyP:25,
    		BracketLeft:26, BracketRight:27, Enter:28, ControlLeft:29,
    		KeyA:30, KeyS:31, KeyD:32, KeyF:33, KeyG:34, KeyH:35, KeyJ:36, KeyK:37, KeyL:38,
    		Semicolon:39, Quote:40, Backquote:41, ShiftLeft:42, Backslash:43,
    		KeyZ:44, KeyX:45, KeyC:46, KeyV:47, KeyB:48, KeyN:49, KeyM:50,
    		Comma:51, Period:52, Slash:53, ShiftRight:54, NumpadMultiply:55, AltLeft:56, Space:57, CapsLock:58,
    		F1:59, F2:60, F3:61, F4:62, F5:63, F6:64, F7:65, F8:66, F9:67, F10:68,
    		NumLock:69, ScrollLock:70, Numpad7:71, Numpad8:72, Numpad9:73, NumpadSubtract:74,
    		Numpad4:75, Numpad5:76, Numpad6:77, NumpadAdd:78, Numpad1:79, Numpad2:80, Numpad3:81, Numpad0:82, NumpadDecimal:83,
    		IntlBackslash:86, F11:87, F12:88, NumpadEnter:96, ControlRight:97, NumpadDivide:98, AltRight:100,
    		Home:102, ArrowUp:103, PageUp:104, ArrowLeft:105, ArrowRight:106, End:107, ArrowDown:108, PageDown:109,
    		Insert:110, Delete:111, NumpadEqual:117, Pause:119
    	};
    	var getLinuxCode=function(e){
    			if( e.code ){													//modern browsers - use the physical key position
    				return codelinux.hasOwnProperty(e.code) ? codelinux[e.code] : -1;
    			}
    			return $.inArray(e.keyCode,keylinux);								//old browsers - fall back to the keyCode table
    		};
    	var onKeyDown=function(e){
    			var iLinuxCode=getLinuxCode(e);
    			//console.log("Key ",e.keyCode);
    			if( iLinuxCode>=0 ){
    				e.preventDefault();
	    			sendKey(iLinuxCode,"down");
	    			iLastSendKeyDown=new Date().getTime();
    			} else{
    				//console.log("Key not found",e.keyCode);
    			}
	    	};
    	var onKeyUp=function(e){
    			var iLinuxCode=getLinuxCode(e);
    			//console.log("Key ",e.keyCode);
    			if( iLinuxCode>=0 ){
    				e.preventDefault();
    				//some touch device keyboard (i.e. iOS) send keydown/keyup 1-4ms apart - thats a bit too fast. 
    				//Keydown/up even could race each other, resulting in keyup arriving first. So we better add a delay if necessary.
	    			var iNow=new Date().getTime();
    				if( iNow-iLastSendKeyDown>20 ){
		    			sendKey(iLinuxCode,"up");
    				} else{
    					window.setTimeout(function(){
    						sendKey(iLinuxCode,"up");
    					},50);
    				}
    			} else{
    				//console.log("Key not found",e.keyCode);
    			}
	    	};
	    var handleStKeyboardKey=function(xThis,e,sState,iX,iY){
        		var iKeysInRow=xThis.data("count") || 1;
        		var lsLinuxKeyCodes=xThis.data("linuxcodes")+"";
        		var asKeyCodes=lsLinuxKeyCodes.split(",");
				if( iKeysInRow==1 ){
					sendKey(asKeyCodes[0]|0,sState); // //important: cast keycode to int
				}else{
					var offset = $("#keyboard").offset();
					var iImageX = offset.left - $(window).scrollLeft();
					var iImageY = offset.top - $(window).scrollTop();
	        		iX-=iImageX;
					iY-=iImageY;
					var liCoords=xThis.attr("coords");
					var aiCoords=liCoords.split(",");
					iX-=aiCoords[0];
					iY-=aiCoords[1];
					var iW=aiCoords[2]-aiCoords[0];
					var iKeyInRow=Math.floor(iX/(iW/iKeysInRow));
					var iKeyCode=asKeyCodes[iKeyInRow]|0; //important: cast keycode to int
					sendKey(iKeyCode,sState);
				}
        	};

	    var sendKey=function(iCode,sState){
	    		var xData={ "type": "pc", "code": iCode, "state":sState };
				//console.log(xData);
				//return;	    		
				$.ajax({
					type: 'POST',
					url: '/hid/keyboard',
					data: JSON.stringify(xData),
					contentType: 'application/json'
				});
			};
		var initPointerLock=function(){
			/* Rresize the canvas to occupy the full page, 
			   by getting the widow width and height and setting it to canvas*/
			 
			//check if we have pointerlock available
			var canvas=$("#canvas").get()[0];
			rpl = 	canvas.requestPointerLock ||
                	canvas.mozRequestPointerLock ||
                	canvas.webkitRequestPointerLock || 
                	null;
            if( rpl!=null ){
            	//register the request function under a normalized name
				canvas.requestPointerLock = rpl;
				// register the callback when a pointerlock event occurs
	        	document.addEventListener('pointerlockchange', onPointerLockchange, false);
	        	document.addEventListener('mozpointerlockchange', onPointerLockchange, false);
	        	document.addEventListener('webkitpointerlockchange', onPointerLockchange, false);   

            	bHasPointerLock=true;
            }else{
            	//no pointerlock - set mock function and disable capability
            	canvas.requestPointerLock=function(){};
            	bHasPointerLock=false;
            }
		};
		//ask browser to ask user to allow hiding of the cursor
		var activatePointerLock=function(){
			//only if we have pointerlock available
			if( bHasPointerLock ){
				var canvas=$("#canvas").get()[0];
				canvas.requestPointerLock();
			}
		};
		//keep track of pointerlock status - mosemove gets different eventobjects when enabled
		var onPointerLockchange=function(e){
			var ple=e.target.pointerLockElement || e.target.mozPointerLockElement || e.target.webkitPointerLockElement || null;
			if( ple==null){
				bPointerLocked=false;
			}else{
				bPointerLocked=true;
			}
			//button stays pressed while grabbed and tells how to get out
			var $btn=$("button#lockpointer");
			if( bPointerLocked ){
				$btn.addClass("grabbed").html("Esc releases mouse");
			}else{
				$btn.removeClass("grabbed").html("Grab mouse");
			}
			fWheelRest=0;
		};
		//mouse wheel while grabbed - don't scroll the page, send cursor up / down to ST instead
		var fWheelRest=0;
		var onWheel=function(e){
			if( !bPointerLocked ){
				return;
			}
			e.preventDefault();
			var fDY=e.deltaY || 0;
			if( e.deltaMode==1 ){			//lines
				fDY*=40;
			}else if( e.deltaMode==2 ){		//pages
				fDY*=400;
			}
			fWheelRest+=fDY;
			//at most one key per wheel event (one per notch on a mouse), touchpads need to scroll a bit for each key
			if( Math.abs(fWheelRest)<30 ){
				return;
			}
			var iCode=(fWheelRest<0) ? 103 : 108;	//linux KEY_UP / KEY_DOWN
			fWheelRest=0;
			sendKey(iCode,"down");
			window.setTimeout(function(){
				sendKey(iCode,"up");
			},30);
		};
		var ignoreEvent=function(e){
        		e.preventDefault();
        		e.stopPropagation();
				return false;	        		
        	};
        var toggleSystemKeyboard=function(e){
        		e.preventDefault();
        		e.stopPropagation();
        		bShowKeyboard=!bShowKeyboard;
        		if(bShowKeyboard){
	        		$("#keyboardtrigger").focus();
        		}else{
	        		$("#keyboardtrigger").blur();
        		}
        		return false;
        	};
        var toggleStKeyboard=function(e){
        		e.preventDefault();
        		e.stopPropagation();
        		bShowStKeyboard=!bShowStKeyboard;
        		if(bShowStKeyboard){
	        		$("#keyboardlayer").show();
        		}else{
	        		$("#keyboardlayer").hide();
        		}
        		updateStKeyboardButton();
        		return false;
        	};
        //button stays pressed while the ST keyboard is shown and tells what a click does
        var updateStKeyboardButton=function(){
        		var $btn=$("button#showstkeyboard");
        		if(bShowStKeyboard){
        			$btn.addClass("pressed").html("Hide ST keyboard");
        		}else{
        			$btn.removeClass("pressed").html("Show ST keyboard");
        		}
        	};
        var toggleHelp=function(e){
        		e.preventDefault();
        		e.stopPropagation();
        		bShowHelp=!bShowHelp;
        		if(bShowHelp){
	        		$(".alert.help.current").show();
        		}else{
	        		$(".alert.help.current").hide();
        		}
        		return false;
        	};
        var closeAlert=function(e){
         		e.preventDefault();
         		e.stopPropagation();
            $(this).parent().hide();
          };
        //bind mouse click to an element in a way, that's not sending an ST click
        var bindClickHelper=function(selector,funcEvent){
            var $element=$(selector);
				    $element.bind('touchstart', funcEvent);
				    $element.bind('touchend', ignoreEvent);
	        	$element.mousedown(funcEvent);
	        	$element.mouseup(ignoreEvent);
	        	$element.click(ignoreEvent);
          };
    	/* ==========================
    	 * 			 public functions
    	 */
    	return {
	        init: function(){
            //remove background if not supported properly
            if( !Modernizr.backgroundsize ){
              $("body").removeClass("background");
            }
	        	//ST keyboard
	        	$("area").mousedown(function(e){
	        		e.preventDefault();
	        		e.stopPropagation();
	        		handleStKeyboardKey($(this),e,"down",e.pageX,e.pageY);
	        	});
	        	$("area").bind("touchstart",function(e){
	        		e.preventDefault();
	        		e.stopPropagation();
	        		handleStKeyboardKey($(this),e,"down",e.changedTouches[0].pageX,e.changedTouches[0].pageY);
	        	});
	        	$("area").mouseup(function(e){
	        		e.preventDefault();
	        		e.stopPropagation();
	        		handleStKeyboardKey($(this),e,"up",e.pageX,e.pageY);
	        	});
	        	$("area").bind("touchend",function(e){
	        		e.preventDefault();
	        		e.stopPropagation();
	        		handleStKeyboardKey($(this),e,"up",e.changedTouches[0].pageX,e.changedTouches[0].pageY);
	        	});
	        	//input pad
	        	$(window).mousemove(onMouseMove);
				    $(document).bind('touchstart', onTouchStart);
				    $(document).bind('touchend', onTouchEnd);
				    $(document).bind('touchmove', onTouchMove);
	        	$(window).mousedown(onMouseDown);
	        	$(window).mouseup(onMouseUp);
	        	$(window).keydown(onKeyDown);
	        	$(window).keyup(onKeyUp);
	        	window.addEventListener('wheel', onWheel, { passive: false });
	        	$(window).doubletap(onDoubleTap,onDoubleTap2Fingers,300);

	        	var bHasTouch = /android|iphone|ipad/i.test(navigator.userAgent.toLowerCase());

            //only if canvas is supported
            var bFeatureMissing=false;
	       	  initPointerLock();
            if( !bHasPointerLock ){
                bFeatureMissing=true;
            }         
            if( bHasTouch ){
                bFeatureMissing=false;
            } 
            if( bFeatureMissing ){
              $("#info-features").removeClass("hidden");
            }

	        	//timer for sending mouse movements ~60 times per second (if previous move was already delivered)
	        	window.setInterval(function(){
	        		if( currentMouseMove!=null && iMouseRequestsInFlight==0 ){
	        			//empty queue before we send, to make sure we don't call the queue with the old value again if this operation takes >50ms for some reason
	        			var x=currentMouseMove;
	        			currentMouseMove=null;
	        			x();
	        		}
	        	}, 16);

	        	//UI
            //Toggle System Keyboard Button
            bindClickHelper("button#showkeyboard",toggleSystemKeyboard);
            //Toggle ST Keyboard Button
            bindClickHelper("button#showstkeyboard",toggleStKeyboard);
            //Help! Button
            bindClickHelper("button#help",toggleHelp);
            //Send CTRL+ALT+DEL
            bindClickHelper("button#keyctrlaltdel",function(e){
            		e.preventDefault();
					e.stopPropagation();
					if( confirm("Really send CTRL-ALT-DEL to the ST?") ){
						sendKey(29,"down");
						sendKey(56,"down");
						sendKey(111,"down");
						setTimeout(function(){
							sendKey(29,"up");
							sendKey(56,"up");
							sendKey(111,"up");
						},100);
					}
                });
            //Start recieving screencast from ST
            bindClickHelper("button#screencast",function(e){
					if( typeof CosmosEx.Screencast!="undefined" && !$(this).hasClass("disabled") ){
						$(this).attr("disabled", "disabled");
						$(this).addClass("disabled");
						$(this).html('Screncast running...');
						CosmosEx.Screencast();
					}
                });

            //close feature bubble on x
            bindClickHelper("#info-features a.close",closeAlert);
            //close help
            bindClickHelper(".alert.help a.close",toggleHelp);
            //Grab mouse Button
            bindClickHelper("button#lockpointer",function(e){
	        		e.preventDefault();
	        		e.stopPropagation();
		        	activatePointerLock();
	        	});
				    if( !bHasPointerLock ){
		          $("button#lockpointer").hide();
				    }
            //links shouldn't send an ST event 
            var $element=$("a");
				    $element.bind('touchstart', function(e){
              $(this).click();
            });
				    $element.bind('touchend', ignoreEvent);
	        	$element.mousedown(ignoreEvent);
	        	$element.mouseup(ignoreEvent);
	        	$element.click(function(e){
	        		e.stopPropagation();
            });

	        	//hide Keyboard toggle on devices!=android/ios
		        if( !bHasTouch ){
		        	$("#showkeyboard").hide();
		        	$("#keyboardtrigger").hide();
		        	$("#keyboardlayer").hide();
		        	bShowStKeyboard=false;
		        }else{
		        	$("button#lockpointer").hide();
		        	bShowStKeyboard=true;
		        }
		        updateStKeyboardButton();
            if( bHasTouch ){
                $(".alert.touch").removeClass("hidden");
                $(".alert.touch").addClass("current");
            } else{
                $(".alert.desktop").removeClass("hidden");
                $(".alert.desktop").addClass("current");
            } 
            bShowHelp=false;

	        	//various browser UI fixes
				    $(document).bind("contextmenu",function(e){
				        return false;
				    });
            //orientation change - prevent the keyboard from getting to large by adjusting the viewport accordingly
            window.addEventListener("orientationchange", function() {
              var w=actual('device-width', 'px');
              if (w < 768) { $('meta[name=viewport]').attr('content','initial-scale='+w/768+', maximum-scale='+w/768+', user-scalable=0'); }
            }, false);
	        },
			onMouseMove: onMouseMove,
			//relative mouse speed multiplier - 1 = unchanged
			setMouseSpeed: function(fSpeed){
				fMouseSpeed=fSpeed;
				fRestDX=0;
				fRestDY=0;
			}
		}
    }();        
