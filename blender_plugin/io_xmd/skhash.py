M=0xffffffff
def rot(x,k): return ((x<<k)|(x>>(32-k)))&M
def lookup3(data,init=0):
    n=len(data); a=b=c=(0xdeadbeef+n+init)&M; p=0
    while n>12:
        a=(a+int.from_bytes(data[p:p+4],'little'))&M; b=(b+int.from_bytes(data[p+4:p+8],'little'))&M; c=(c+int.from_bytes(data[p+8:p+12],'little'))&M
        a=(a-c)&M; a^=rot(c,4); c=(c+b)&M
        b=(b-a)&M; b^=rot(a,6); a=(a+c)&M
        c=(c-b)&M; c^=rot(b,8); b=(b+a)&M
        a=(a-c)&M; a^=rot(c,16); c=(c+b)&M
        b=(b-a)&M; b^=rot(a,19); a=(a+c)&M
        c=(c-b)&M; c^=rot(b,4); b=(b+a)&M
        p+=12; n-=12
    if n==0: return c
    t=data[p:]+b'\0'*12
    a=(a+int.from_bytes(t[0:4],'little'))&M; b=(b+int.from_bytes(t[4:8],'little'))&M; c=(c+int.from_bytes(t[8:12],'little'))&M
    c^=b; c=(c-rot(b,14))&M
    a^=c; a=(a-rot(c,11))&M
    b^=a; b=(b-rot(a,25))&M
    c^=b; c=(c-rot(b,16))&M
    a^=c; a=(a-rot(c,4))&M
    b^=a; b=(b-rot(a,14))&M
    c^=b; c=(c-rot(b,24))&M
    return c
def h(s,seed=0): return lookup3(s.upper().encode(),seed)
if __name__=='__main__':
    import sys
    for s in sys.argv[1:]: print(s,'%08X'%h(s))
