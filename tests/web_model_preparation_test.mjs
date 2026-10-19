// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {runInNewContext} from 'node:vm';
import {test} from 'node:test';
const source=readFileSync(new URL('../assets/web/app.js',import.meta.url),'utf8');
const code=source.slice(source.indexOf("let preparedModel="),source.indexOf("$('search').oninput=renderModels"));
function fixture(){
 const elements=new Map(),requests=[];
 const c={current:{model:'text',messages:[]},currentModelChosen:true,selectedModel:'',models:[{id:'text',name:'Text'}],conversations:[],Date,
 $:id=>{if(!elements.has(id))elements.set(id,{textContent:'',disabled:false,value:''});return elements.get(id);},
 refreshMemoryModel(){},modelChooser:{value:'text',disabled:false},newConversation(){},openChat(){},renderModels(){},renderHistory(){},persistConversation(){},
 async api(path,options){requests.push(JSON.parse(options.body));return {ok:true,json:async()=>({state:options.body.includes('true')?'ready':'selected'})};}};
 runInNewContext(code,c);return {c,requests};
}
test('selecting defers loading; concurrent preparation requests load only once',async()=>{
 const {c,requests}=fixture();await c.chooseInstalledModel('text');
 assert.equal(requests.length,1);assert.equal(requests[0].prepare,false);
 let release; c.api=async(path,options)=>{requests.push(JSON.parse(options.body));await new Promise(resolve=>{release=resolve;});return {ok:true,json:async()=>({state:'ready'})};};
 const first=c.prepareConversationModel(),second=c.prepareConversationModel();
 assert.equal(requests.length,2);assert.equal(requests[1].prepare,true);
 release();await Promise.all([first,second]);await c.prepareConversationModel();assert.equal(requests.length,2);
});
test('failed preparation can be retried and restores model selector',async()=>{
 const {c}=fixture();let calls=0;c.api=async()=>{const ok=++calls>1;return {ok,json:async()=>ok?({state:'ready'}):({error:'prepare failed'})};};
 await assert.rejects(c.prepareConversationModel());assert.equal(c.modelChooser.disabled,false);
 await c.prepareConversationModel();assert.equal(calls,2);
});
