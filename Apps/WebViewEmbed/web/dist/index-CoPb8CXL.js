(function(){const r=document.createElement("link").relList;if(r&&r.supports&&r.supports("modulepreload"))return;for(const e of document.querySelectorAll('link[rel="modulepreload"]'))n(e);new MutationObserver(e=>{for(const t of e)if(t.type==="childList")for(const o of t.addedNodes)o.tagName==="LINK"&&o.rel==="modulepreload"&&n(o)}).observe(document,{childList:!0,subtree:!0});function c(e){const t={};return e.integrity&&(t.integrity=e.integrity),e.referrerPolicy&&(t.referrerPolicy=e.referrerPolicy),e.crossOrigin==="use-credentials"?t.credentials="include":e.crossOrigin==="anonymous"?t.credentials="omit":t.credentials="same-origin",t}function n(e){if(e.ep)return;e.ep=!0;const t=c(e);fetch(e.href,t)}})();const i=document.querySelector("#app");i.innerHTML=`
    <h1>Hello from Vite + eacp</h1>
    <p>This page is served by Vite in dev and embedded via ResEmbed in release.</p>
    <button id="ping">Ping</button>
    <div id="out"></div>
`;const s=i.querySelector("#out");i.querySelector("#ping").addEventListener("click",()=>{s.textContent=`clicked at ${new Date().toLocaleTimeString()}`});
