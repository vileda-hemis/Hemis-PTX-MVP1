// Executes the /register walkthrough's render() against a DOM shim and asserts what it
// EMITS -- not merely that it parses.  ODC-098: the check must be on the thing itself.
// Argument order is asserted against `Hemis-cli help protx_register` (v0.3.1-testnet).
var fs=require("fs"),path=require("path");
var app=fs.readFileSync(path.join(__dirname,"app.py"),"utf8");
var html=/REGISTER_HTML = r"""([\s\S]*?)"""/.exec(app)[1];
var js=/<script>([\s\S]*?)<\/script>/.exec(html)[1];

var els={},fail=0,pass=0;
function E(id){return {id:id,value:"",checked:false,className:"",innerHTML:"",
  addEventListener:function(){},querySelector:function(){return null}}}
global.document={getElementById:function(id){return els[id]||(els[id]=E(id))}};
global.window={};global.navigator={clipboard:{writeText:function(){return{then:function(){}}}}};
eval(js);

function reset(){els={};["name","coladdr","ctxid","cvout","owner","payout","samepay",
  "blspub","ipport","compound"].forEach(function(i){els[i]=E(i)});}
function good(){reset();
  els.name.value="gm01";els.coladdr.value="yCOLL111";els.ctxid.value="a".repeat(64);
  els.cvout.value="1";els.owner.value="yOWN222";els.payout.value="yPAY333";
  els.blspub.value="bls-pk-ptx1qqq";els.ipport.value="[2a07::1]:29994";els.samepay.checked=true;}
function strip(h){return h.replace(/<[^>]*>/g,"").replace(/&quot;/g,'"').replace(/&amp;/g,"&")}
function argv(){var c=strip(els.cmd5.innerHTML).replace(/\s*copy\s*$/,"")
  .replace(/\\\n\s*/g," ").replace(/^\s*wallet host\s*/i,"").trim();
  return c.match(/"[^"]*"|\S+/g)||[]}
function t(name,fn){try{fn();pass++;console.log("  ok   "+name)}
  catch(e){fail++;console.log("  FAIL "+name+" -- "+e.message)}}
function eq(a,b,m){if(a!==b)throw new Error((m||"")+" expected "+JSON.stringify(b)+" got "+JSON.stringify(a))}
function on(id){if(els[id].className.indexOf("off")>=0)throw new Error(id+" should be enabled")}
function isoff(id){if(els[id].className.indexOf("off")<0)throw new Error(id+" should be disabled")}

console.log("register walkthrough:");

t("emits 11 arguments in help-text order",function(){
  good();render();var a=argv();
  eq(a.length,13,"argc");
  eq(a[1],"protx_register");
  eq(a[2],'"'+"a".repeat(64)+'"',"arg1 collateralHash");
  eq(a[3],"1","arg2 collateralIndex");
  eq(a[4],'"[2a07::1]:29994"',"arg3 ipAndPort");
  eq(a[5],'"yOWN222"',"arg4 ownerAddress");
  eq(a[6],'"bls-pk-ptx1qqq"',"arg5 operatorPubKey");
  eq(a[7],'""',"arg6 votingAddress defaults to owner");
  eq(a[8],'"yPAY333"',"arg7 payoutAddress");
  eq(a[9],"0","arg8 operatorReward");
  eq(a[10],'""',"arg9 operatorPayoutAddress");
  eq(a[11],'"yPAY333"',"arg10 ptxPaymentAddress defaults to payout");
  eq(a[12],'"gm01"',"arg11 ptxNodeId is the bare label");
});

t("ptxNodeId is the bare label, never compound",function(){
  good();render();
  if(argv()[12].indexOf(":")>=0)throw new Error("sent a compound label; the chain appends the suffix");
});

t("opt-out leaves arg10 empty and does NOT block",function(){
  good();els.samepay.checked=false;window._pp="";render();
  on("s5");eq(argv()[11],'""',"arg10");
  if(els.cmd5note.innerHTML.indexOf("not eligible")<0)throw new Error("no consequence stated");
  if(els.cmd5note.innerHTML.indexOf("re-registering is the only fix")<0)throw new Error("no remedy stated");
});

t("collateral must come before the keys",function(){
  good();els.coladdr.value="";render();isoff("s3");isoff("s4");isoff("s5");
});

t("owner equal to collateral is refused",function(){
  good();els.owner.value="yCOLL111";render();
  isoff("s5");
  if(els.s4msg.innerHTML.indexOf("differ from the collateral")<0)throw new Error("rule not named");
});

t("owner equal to payout is ALLOWED (chain does not forbid it)",function(){
  good();els.owner.value="yPAY333";render();on("s5");
});

t("secret BLS key is caught",function(){
  good();els.blspub.value="bls-sk-ptx1zzz";render();
  isoff("s5");
  if(els.s4msg.innerHTML.indexOf("SECRET")<0)throw new Error("rule not named");
});

t("label rules name the failing rule",function(){
  reset();els.name.value="admin";render();
  if(els.namemsg.innerHTML.indexOf("reserved word")<0)throw new Error("reserved not named");
  reset();els.name.value="12345";render();
  if(els.namemsg.innerHTML.indexOf("all digits")<0)throw new Error("all-digits not named");
  reset();els.name.value="gm!1";render();
  if(els.namemsg.innerHTML.indexOf("Only letters")<0)throw new Error("charset not named");
});

t("bare label pasted as compound is refused",function(){
  good();els.compound.value="gm01";render();
  if(els.cmd6.innerHTML.indexOf("bare label")<0)throw new Error("not caught");
  if(els.cmd6.innerHTML.indexOf("ptxnodeid=")>=0)throw new Error("emitted a config line anyway");
});

t("compound from a different gamemaster is refused",function(){
  good();els.compound.value="gm02:d7b70a85";render();
  if(els.cmd6.innerHTML.indexOf("but this walkthrough registered")<0)throw new Error("not caught");
});

t("good compound emits the config line",function(){
  good();els.compound.value="gm01:d7b70a85";render();
  if(els.cmd6.innerHTML.indexOf("ptxnodeid=gm01:d7b70a85")<0)throw new Error("config line missing");
});

t("page never emits a network call",function(){
  if(/fetch\(|XMLHttpRequest|WebSocket|EventSource|navigator\.sendBeacon/.test(js))
    throw new Error("page contains a network primitive");
});

t("the guide and the page emit the same argument order",function(){
  // ★★ Two documents describing one command is how the disagreement starts.
  // OPERATOR_GUIDE.md sec B2 carries a positional table; the page emits a command.
  // Assert they are the same sequence, so neither can drift alone.
  var g=path.join(__dirname,"..","operator","OPERATOR_GUIDE.md");
  if(!fs.existsSync(g))throw new Error("NOT PERFORMED: OPERATOR_GUIDE.md not found");
  var md=fs.readFileSync(g,"utf8");
  var sec=/### B2\. Register([\s\S]*?)\n### /.exec(md);
  if(!sec)throw new Error("NOT PERFORMED: section B2 not found");
  var rows=sec[1].match(/^\| *(\d+) *\| *`([^`]+)` *\|/gm)||[];
  if(rows.length!==11)throw new Error("guide table has "+rows.length+" rows, expected 11");
  var names=rows.map(function(r){return /`([^`]+)`/.exec(r)[1]});
  var expect=["collateralHash","collateralIndex","ipAndPort","ownerAddress","operatorPubKey",
              "votingAddress","payoutAddress","operatorReward","operatorPayoutAddress",
              "ptxPaymentAddress","ptxNodeId"];
  for(var i=0;i<11;i++) eq(names[i],expect[i],"guide row "+(i+1));
  // and the page must put the same VALUES in those slots
  good();render();var a=argv();
  eq(a.length,13,"page argc must equal the guide's 11 + rpc + binary");
  eq(a[7],'""',"guide row 6 votingAddress empty <-> page arg 6");
  eq(a[9],"0","guide row 8 operatorReward 0 <-> page arg 8");
  eq(a[10],'""',"guide row 9 operatorPayoutAddress empty <-> page arg 9");
});

console.log("\n"+pass+" passed, "+fail+" failed");
process.exit(fail?1:0);
