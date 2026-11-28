"""Small 3x3 rotation / quaternion helpers (column-vector convention, quaternions as (x, y, z, w))."""
import math
def R(q):
    x, y, z, w = q
    return [[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
            [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
            [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]]
def mul(a, b): return [[sum(a[i][k]*b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
def T(a): return [list(r) for r in zip(*a)]
def mv(a, v): return [sum(a[i][k]*v[k] for k in range(3)) for i in range(3)]
def add(a, b): return [a[i]+b[i] for i in range(3)]
def Q(m):  # matrix -> quaternion (x,y,z,w)
    t = m[0][0]+m[1][1]+m[2][2]
    if t > 0:
        s = math.sqrt(t+1)*2; return ((m[2][1]-m[1][2])/s, (m[0][2]-m[2][0])/s, (m[1][0]-m[0][1])/s, s/4)
    i = max(range(3), key=lambda k: m[k][k]); j, k = (i+1) % 3, (i+2) % 3
    s = math.sqrt(1+m[i][i]-m[j][j]-m[k][k])*2
    q = [0, 0, 0, 0]; q[i] = s/4; q[j] = (m[j][i]+m[i][j])/s; q[k] = (m[k][i]+m[i][k])/s; q[3] = (m[k][j]-m[j][k])/s
    return tuple(q)
